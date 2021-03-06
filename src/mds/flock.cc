// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
#include <errno.h>

#include "common/debug.h"
#include "mdstypes.h"
#include "mds/flock.h"

#define dout_subsys ceph_subsys_mds

bool ceph_lock_state_t::is_waiting(ceph_filelock &fl)
{
  multimap<uint64_t, ceph_filelock>::iterator p = waiting_locks.find(fl.start);
  while (p != waiting_locks.end()) {
    if (p->second.start > fl.start)
      return false;
    if (p->second.length == fl.length &&
        p->second.client == fl.client &&
        p->second.pid == fl.pid &&
        p->second.pid_namespace == fl.pid_namespace)
      return true;
    ++p;
  }
  return false;
}

void ceph_lock_state_t::remove_waiting(ceph_filelock& fl)
{
  multimap<uint64_t, ceph_filelock>::iterator p = waiting_locks.find(fl.start);
  while (p != waiting_locks.end()) {
    if (p->second.start > fl.start)
      return;
    if (p->second.length == fl.length &&
        p->second.client == fl.client &&
        p->second.pid == fl.pid &&
        p->second.pid_namespace == fl.pid_namespace) {
      waiting_locks.erase(p);
      --client_waiting_lock_counts[(client_t)fl.client];
      if (!client_waiting_lock_counts[(client_t)fl.client]) {
        client_waiting_lock_counts.erase((client_t)fl.client);
      }
      return;
    }
    ++p;
  }
}

bool ceph_lock_state_t::add_lock(ceph_filelock& new_lock,
                                 bool wait_on_fail, bool replay)
{
  dout(15) << "add_lock " << new_lock << dendl;
  bool ret = false;
  list<multimap<uint64_t, ceph_filelock>::iterator>
    overlapping_locks, self_overlapping_locks, neighbor_locks;

  // first, get any overlapping locks and split them into owned-by-us and not
  if (get_overlapping_locks(new_lock, overlapping_locks, &neighbor_locks)) {
    dout(15) << "got overlapping lock, splitting by owner" << dendl;
    split_by_owner(new_lock, overlapping_locks, self_overlapping_locks);
  }
  if (!overlapping_locks.empty()) { //overlapping locks owned by others :(
    if (CEPH_LOCK_EXCL == new_lock.type) {
      //can't set, we want an exclusive
      dout(15) << "overlapping lock, and this lock is exclusive, can't set"
              << dendl;
      if (wait_on_fail && !replay) {
        waiting_locks.insert(pair<uint64_t, ceph_filelock>(new_lock.start, new_lock));
      }
    } else { //shared lock, check for any exclusive locks blocking us
      if (contains_exclusive_lock(overlapping_locks)) { //blocked :(
        dout(15) << " blocked by exclusive lock in overlapping_locks" << dendl;
        if (wait_on_fail && !replay) {
          waiting_locks.insert(pair<uint64_t, ceph_filelock>(new_lock.start, new_lock));
        }
      } else {
        //yay, we can insert a shared lock
        dout(15) << "inserting shared lock" << dendl;
        remove_waiting(new_lock);
        adjust_locks(self_overlapping_locks, new_lock, neighbor_locks);
        held_locks.insert(pair<uint64_t, ceph_filelock>(new_lock.start, new_lock));
        ret = true;
      }
    }
  } else { //no overlapping locks except our own
    remove_waiting(new_lock);
    adjust_locks(self_overlapping_locks, new_lock, neighbor_locks);
    dout(15) << "no conflicts, inserting " << new_lock << dendl;
    held_locks.insert(pair<uint64_t, ceph_filelock>
                      (new_lock.start, new_lock));
    ret = true;
  }
  if (ret) {
    ++client_held_lock_counts[(client_t)new_lock.client];
  }
  else if (wait_on_fail && !replay)
    ++client_waiting_lock_counts[(client_t)new_lock.client];
  return ret;
}

void ceph_lock_state_t::look_for_lock(ceph_filelock& testing_lock)
{
  list<multimap<uint64_t, ceph_filelock>::iterator> overlapping_locks,
    self_overlapping_locks;
  if (get_overlapping_locks(testing_lock, overlapping_locks)) {
    split_by_owner(testing_lock, overlapping_locks, self_overlapping_locks);
  }
  if (!overlapping_locks.empty()) { //somebody else owns overlapping lock
    if (CEPH_LOCK_EXCL == testing_lock.type) { //any lock blocks it
      testing_lock = (*overlapping_locks.begin())->second;
    } else {
      ceph_filelock *blocking_lock;
      if ((blocking_lock = contains_exclusive_lock(overlapping_locks))) {
        testing_lock = *blocking_lock;
      } else { //nothing blocking!
        testing_lock.type = CEPH_LOCK_UNLOCK;
      }
    }
    return;
  }
  //if we get here, only our own locks block
  testing_lock.type = CEPH_LOCK_UNLOCK;
}

void ceph_lock_state_t::remove_lock(ceph_filelock removal_lock,
                 list<ceph_filelock>& activated_locks)
{
  list<multimap<uint64_t, ceph_filelock>::iterator> overlapping_locks,
    self_overlapping_locks;
  if (get_overlapping_locks(removal_lock, overlapping_locks)) {
    dout(15) << "splitting by owner" << dendl;
    split_by_owner(removal_lock, overlapping_locks, self_overlapping_locks);
  } else dout(15) << "attempt to remove lock at " << removal_lock.start
                 << " but no locks there!" << dendl;
  bool remove_to_end = (0 == removal_lock.length);
  uint64_t removal_start = removal_lock.start;
  uint64_t removal_end = removal_start + removal_lock.length - 1;
  uint64_t old_lock_end;
  __s64 old_lock_client = 0;
  ceph_filelock *old_lock;

  dout(15) << "examining " << self_overlapping_locks.size()
          << " self-overlapping locks for removal" << dendl;
  for (list<multimap<uint64_t, ceph_filelock>::iterator>::iterator
         iter = self_overlapping_locks.begin();
       iter != self_overlapping_locks.end();
       ++iter) {
    dout(15) << "self overlapping lock " << (*iter)->second << dendl;
    old_lock = &(*iter)->second;
    bool old_lock_to_end = (0 == old_lock->length);
    old_lock_end = old_lock->start + old_lock->length - 1;
    old_lock_client = old_lock->client;
    if (remove_to_end) {
      if (old_lock->start < removal_start) {
        old_lock->length = removal_start - old_lock->start;
      } else {
        dout(15) << "erasing " << (*iter)->second << dendl;
        held_locks.erase(*iter);
        --client_held_lock_counts[old_lock_client];
      }
    } else if (old_lock_to_end) {
      ceph_filelock append_lock = *old_lock;
      append_lock.start = removal_end+1;
      held_locks.insert(pair<uint64_t, ceph_filelock>
                        (append_lock.start, append_lock));
      ++client_held_lock_counts[(client_t)old_lock->client];
      if (old_lock->start >= removal_start) {
        dout(15) << "erasing " << (*iter)->second << dendl;
        held_locks.erase(*iter);
        --client_held_lock_counts[old_lock_client];
      } else old_lock->length = removal_start - old_lock->start;
    } else {
      if (old_lock_end  > removal_end) {
        ceph_filelock append_lock = *old_lock;
        append_lock.start = removal_end + 1;
        append_lock.length = old_lock_end - append_lock.start + 1;
        held_locks.insert(pair<uint64_t, ceph_filelock>
                          (append_lock.start, append_lock));
        ++client_held_lock_counts[(client_t)old_lock->client];
      }
      if (old_lock->start < removal_start) {
        old_lock->length = removal_start - old_lock->start;
      } else {
        dout(15) << "erasing " << (*iter)->second << dendl;
        held_locks.erase(*iter);
        --client_held_lock_counts[old_lock_client];
      }
    }
    if (!client_held_lock_counts[old_lock_client]) {
      client_held_lock_counts.erase(old_lock_client);
    }
  }
}

bool ceph_lock_state_t::remove_all_from (client_t client)
{
  bool cleared_any = false;
  if (client_held_lock_counts.count(client)) {
    remove_all_from(client, held_locks);
    client_held_lock_counts.erase(client);
    cleared_any = true;
  }
  if (client_waiting_lock_counts.count(client)) {
    remove_all_from(client, waiting_locks);
    client_waiting_lock_counts.erase(client);
  }
  return cleared_any;
}

void ceph_lock_state_t::adjust_locks(list<multimap<uint64_t, ceph_filelock>::iterator> old_locks,
                  ceph_filelock& new_lock,
                  list<multimap<uint64_t, ceph_filelock>::iterator>
                  neighbor_locks)
{
  dout(15) << "adjust_locks" << dendl;
  bool new_lock_to_end = (0 == new_lock.length);
  uint64_t new_lock_start = new_lock.start;
  uint64_t new_lock_end = new_lock.start + new_lock.length - 1;
  uint64_t old_lock_start, old_lock_end;
  __s64 old_lock_client = 0;
  ceph_filelock *old_lock;
  for (list<multimap<uint64_t, ceph_filelock>::iterator>::iterator
         iter = old_locks.begin();
       iter != old_locks.end();
       ++iter) {
    old_lock = &(*iter)->second;
    dout(15) << "adjusting lock: " << *old_lock << dendl;
    bool old_lock_to_end = (0 == old_lock->length);
    old_lock_start = old_lock->start;
    old_lock_end = old_lock->start + old_lock->length - 1;
    new_lock_start = new_lock.start;
    new_lock_end = new_lock.start + new_lock.length - 1;
    old_lock_client = old_lock->client;
    if (new_lock_to_end || old_lock_to_end) {
      //special code path to deal with a length set at 0
      dout(15) << "one lock extends forever" << dendl;
      if (old_lock->type == new_lock.type) {
        //just unify them in new lock, remove old lock
        dout(15) << "same lock type, unifying" << dendl;
        new_lock.start = (new_lock_start < old_lock_start) ? new_lock_start :
          old_lock_start;
        new_lock.length = 0;
        held_locks.erase(*iter);
        --client_held_lock_counts[old_lock_client];
      } else { //not same type, have to keep any remains of old lock around
        dout(15) << "shrinking old lock" << dendl;
        if (new_lock_to_end) {
          if (old_lock_start < new_lock_start) {
            old_lock->length = new_lock_start - old_lock_start;
          } else {
            held_locks.erase(*iter);
            --client_held_lock_counts[old_lock_client];
          }
        } else { //old lock extends past end of new lock
          ceph_filelock appended_lock = *old_lock;
          appended_lock.start = new_lock_end + 1;
          held_locks.insert(pair<uint64_t, ceph_filelock>
                            (appended_lock.start, appended_lock));
          ++client_held_lock_counts[(client_t)old_lock->client];
          if (old_lock_start < new_lock_start) {
            old_lock->length = new_lock_start - old_lock_start;
          } else {
            held_locks.erase(*iter);
            --client_held_lock_counts[old_lock_client];
          }
        }
      }
    } else {
      if (old_lock->type == new_lock.type) { //just merge them!
        dout(15) << "merging locks, they're the same type" << dendl;
        new_lock.start = (old_lock_start < new_lock_start ) ? old_lock_start :
          new_lock_start;
        int new_end = (new_lock_end > old_lock_end) ? new_lock_end :
          old_lock_end;
        new_lock.length = new_end - new_lock.start + 1;
        dout(15) << "erasing lock " << (*iter)->second << dendl;
        held_locks.erase(*iter);
        --client_held_lock_counts[old_lock_client];
      } else { //we'll have to update sizes and maybe make new locks
        dout(15) << "locks aren't same type, changing sizes" << dendl;
        if (old_lock_end > new_lock_end) { //add extra lock after new_lock
          ceph_filelock appended_lock = *old_lock;
          appended_lock.start = new_lock_end + 1;
          appended_lock.length = old_lock_end - appended_lock.start + 1;
          held_locks.insert(pair<uint64_t, ceph_filelock>
                            (appended_lock.start, appended_lock));
          ++client_held_lock_counts[(client_t)old_lock->client];
        }
        if (old_lock_start < new_lock_start) {
          old_lock->length = new_lock_start - old_lock_start;
        } else { //old_lock starts inside new_lock, so remove it
          //if it extended past new_lock_end it's been replaced
          held_locks.erase(*iter);
          --client_held_lock_counts[old_lock_client];
        }
      }
    }
    if (!client_held_lock_counts[old_lock_client]) {
      client_held_lock_counts.erase(old_lock_client);
    }
  }

  //make sure to coalesce neighboring locks
  for (list<multimap<uint64_t, ceph_filelock>::iterator>::iterator
         iter = neighbor_locks.begin();
       iter != neighbor_locks.end();
       ++iter) {
    old_lock = &(*iter)->second;
    old_lock_client = old_lock->client;
    dout(15) << "lock to coalesce: " << *old_lock << dendl;
    /* because if it's a neighboring lock there can't be any self-overlapping
       locks that covered it */
    if (old_lock->type == new_lock.type) { //merge them
      if (0 == new_lock.length) {
        if (old_lock->start + old_lock->length == new_lock.start) {
          new_lock.start = old_lock->start;
        } else assert(0); /* if there's no end to new_lock, the neighbor
                             HAS TO be to left side */
      } else if (0 == old_lock->length) {
        if (new_lock.start + new_lock.length == old_lock->start) {
          new_lock.length = 0;
        } else assert(0); //same as before, but reversed
      } else {
        if (old_lock->start + old_lock->length == new_lock.start) {
          new_lock.start = old_lock->start;
          new_lock.length = old_lock->length + new_lock.length;
        } else if (new_lock.start + new_lock.length == old_lock->start) {
          new_lock.length = old_lock->length + new_lock.length;
        }
      }
      held_locks.erase(*iter);
      --client_held_lock_counts[old_lock_client];
    }
    if (!client_held_lock_counts[old_lock_client]) {
      client_held_lock_counts.erase(old_lock_client);
    }
  }
}

void ceph_lock_state_t::remove_all_from(client_t client,
                                        multimap<uint64_t,
                                          ceph_filelock>& locks)
{
  multimap<uint64_t, ceph_filelock>::iterator iter = locks.begin();
  while (iter != locks.end()) {
    if ((client_t)iter->second.client == client) {
      locks.erase(iter++);
    } else ++iter;
  }
}

multimap<uint64_t, ceph_filelock>::iterator
ceph_lock_state_t::get_lower_bound(uint64_t start,
                                   multimap<uint64_t, ceph_filelock>& lock_map)
{
   multimap<uint64_t, ceph_filelock>::iterator lower_bound =
     lock_map.lower_bound(start);
   if ((lower_bound->first != start)
       && (start != 0)
       && (lower_bound != lock_map.begin())) --lower_bound;
   if (lock_map.end() == lower_bound)
     dout(15) << "get_lower_dout(15)eturning end()" << dendl;
   else dout(15) << "get_lower_bound returning iterator pointing to "
                << lower_bound->second << dendl;
   return lower_bound;
 }

multimap<uint64_t, ceph_filelock>::iterator
ceph_lock_state_t::get_last_before(uint64_t end,
                                   multimap<uint64_t, ceph_filelock>& lock_map)
{
  multimap<uint64_t, ceph_filelock>::iterator last =
    lock_map.upper_bound(end);
  if (last != lock_map.begin()) --last;
  if (lock_map.end() == last)
    dout(15) << "get_last_before returning end()" << dendl;
  else dout(15) << "get_last_before returning iterator pointing to "
               << last->second << dendl;
  return last;
}

bool ceph_lock_state_t::share_space(
    multimap<uint64_t, ceph_filelock>::iterator& iter,
    uint64_t start, uint64_t end)
{
  bool ret = ((iter->first >= start && iter->first <= end) ||
              ((iter->first < start) &&
               (((iter->first + iter->second.length - 1) >= start) ||
                (0 == iter->second.length))));
  dout(15) << "share_space got start: " << start << ", end: " << end
          << ", lock: " << iter->second << ", returning " << ret << dendl;
  return ret;
}

bool ceph_lock_state_t::get_overlapping_locks(ceph_filelock& lock,
                           list<multimap<uint64_t,
                               ceph_filelock>::iterator> & overlaps,
                           list<multimap<uint64_t,
                               ceph_filelock>::iterator> *self_neighbors)
{
  dout(15) << "get_overlapping_locks" << dendl;
  // create a lock starting one earlier and ending one later
  // to check for neighbors
  ceph_filelock neighbor_check_lock = lock;
  if (neighbor_check_lock.start != 0) {
    neighbor_check_lock.start = neighbor_check_lock.start - 1;
    if (neighbor_check_lock.length)
      neighbor_check_lock.length = neighbor_check_lock.length + 2;
  } else {
    if (neighbor_check_lock.length)
      neighbor_check_lock.length = neighbor_check_lock.length + 1;
  }
  //find the last held lock starting at the point after lock
  uint64_t endpoint = lock.start;
  if (lock.length) {
    endpoint += lock.length;
  } else {
    endpoint = uint64_t(-1); // max offset
  }
  multimap<uint64_t, ceph_filelock>::iterator iter =
    get_last_before(endpoint, held_locks);
  bool cont = iter != held_locks.end();
  while(cont) {
    if (share_space(iter, lock)) {
      overlaps.push_front(iter);
    } else if (self_neighbors &&
               (neighbor_check_lock.client == iter->second.client) &&
               (neighbor_check_lock.pid == iter->second.pid) &&
               share_space(iter, neighbor_check_lock)) {
      self_neighbors->push_front(iter);
    }
    if ((iter->first < lock.start) && (CEPH_LOCK_EXCL == iter->second.type)) {
      //can't be any more overlapping locks or they'd interfere with this one
      cont = false;
    } else if (held_locks.begin() == iter) cont = false;
    else --iter;
  }
  return !overlaps.empty();
}

bool ceph_lock_state_t::get_waiting_overlaps(ceph_filelock& lock,
                                             list<multimap<uint64_t,
                                               ceph_filelock>::iterator>&
                                               overlaps)
{
  dout(15) << "get_waiting_overlaps" << dendl;
  multimap<uint64_t, ceph_filelock>::iterator iter =
    get_last_before(lock.start + lock.length - 1, waiting_locks);
  bool cont = iter != waiting_locks.end();
  while(cont) {
    if (share_space(iter, lock)) overlaps.push_front(iter);
    if (waiting_locks.begin() == iter) cont = false;
    --iter;
  }
  return !overlaps.empty();
}

void ceph_lock_state_t::split_by_owner(ceph_filelock& owner,
                                       list<multimap<uint64_t,
                                           ceph_filelock>::iterator>& locks,
                                       list<multimap<uint64_t,
                                           ceph_filelock>::iterator>&
                                           owned_locks)
{
  list<multimap<uint64_t, ceph_filelock>::iterator>::iterator
    iter = locks.begin();
  dout(15) << "owner lock: " << owner << dendl;
  while (iter != locks.end()) {
    dout(15) << "comparing to " << (*iter)->second << dendl;
    if ((*iter)->second.client == owner.client &&
        (*iter)->second.pid_namespace == owner.pid_namespace &&
        (*iter)->second.pid == owner.pid) {
      dout(15) << "success, pushing to owned_locks" << dendl;
      owned_locks.push_back(*iter);
      iter = locks.erase(iter);
    } else {
      dout(15) << "failure, something not equal in this group "
              << (*iter)->second.client << ":" << owner.client << ","
              << (*iter)->second.pid_namespace << ":" << owner.pid_namespace
              << "," << (*iter)->second.pid << ":" << owner.pid << dendl;
      ++iter;
    }
  }
}

ceph_filelock *
ceph_lock_state_t::contains_exclusive_lock(list<multimap<uint64_t,
                                               ceph_filelock>::iterator>& locks)
{
  for (list<multimap<uint64_t, ceph_filelock>::iterator>::iterator
         iter = locks.begin();
       iter != locks.end();
       ++iter) {
    if (CEPH_LOCK_EXCL == (*iter)->second.type) return &(*iter)->second;
  }
  return NULL;
}
