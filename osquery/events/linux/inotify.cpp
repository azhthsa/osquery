/**
 *  Copyright (c) 2014-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under both the Apache 2.0 license (found in the
 *  LICENSE file in the root directory of this source tree) and the GPLv2 (found
 *  in the COPYING file in the root directory of this source tree).
 *  You may select, at your option, one of the above-listed licenses.
 */

#include <sstream>

#include <fnmatch.h>
#include <linux/limits.h>
#include <poll.h>

#include <boost/filesystem.hpp>

#include <osquery/config.h>
#include <osquery/filesystem.h>
#include <osquery/logger.h>
#include <osquery/system.h>

#include "osquery/events/linux/inotify.h"

namespace fs = boost::filesystem;

namespace osquery {

static const size_t kINotifyMaxEvents = 512;
static const size_t kINotifyEventSize =
    sizeof(struct inotify_event) + (NAME_MAX + 1);
static const size_t kINotifyBufferSize =
    (kINotifyMaxEvents * kINotifyEventSize);

std::map<int, std::string> kMaskActions = {
    {IN_ACCESS, "ACCESSED"},
    {IN_ATTRIB, "ATTRIBUTES_MODIFIED"},
    {IN_CLOSE_WRITE, "UPDATED"},
    {IN_CREATE, "CREATED"},
    {IN_DELETE, "DELETED"},
    {IN_MODIFY, "UPDATED"},
    {IN_MOVED_FROM, "MOVED_FROM"},
    {IN_MOVED_TO, "MOVED_TO"},
    {IN_OPEN, "OPENED"},
};

const uint32_t kFileDefaultMasks = IN_MOVED_TO | IN_MOVED_FROM | IN_MODIFY |
                                   IN_DELETE | IN_CREATE | IN_CLOSE_WRITE |
                                   IN_ATTRIB;
const uint32_t kFileAccessMasks = IN_OPEN | IN_ACCESS;

REGISTER(INotifyEventPublisher, "event_publisher", "inotify");

Status INotifyEventPublisher::setUp() {
  inotify_handle_ = ::inotify_init();
  // If this does not work throw an exception.
  if (inotify_handle_ == -1) {
    return Status(1, "Could not start inotify: inotify_init failed");
  }

  WriteLock lock(scratch_mutex_);
  scratch_ = (char*)malloc(kINotifyBufferSize);
  if (scratch_ == nullptr) {
    return Status(1, "Could not allocate scratch space");
  }
  return Status(0, "OK");
}

bool INotifyEventPublisher::needMonitoring(const std::string& path,
                                           INotifySubscriptionContextRef& isc,
                                           uint32_t mask,
                                           bool recursive,
                                           bool add_watch) {
  bool rc = true;
  struct stat file_dir_stat;
  time_t sc_time = isc->path_sc_time_[path];

  if (stat(path.c_str(), &file_dir_stat) == -1) {
    LOG(WARNING) << "Failed to do stat on: " << path;
    return false;
  }

  if (sc_time != file_dir_stat.st_ctime) {
    if ((rc = addMonitor(path, isc, isc->mask, isc->recursive, add_watch))) {
      isc->path_sc_time_[path] = file_dir_stat.st_ctime;
    }
  }
  return rc;
}

bool INotifyEventPublisher::monitorSubscription(
    INotifySubscriptionContextRef& sc, bool add_watch) {
  std::string discovered = sc->path;
  if (sc->path.find("**") != std::string::npos) {
    sc->recursive = true;
    discovered = sc->path.substr(0, sc->path.find("**"));
    sc->path = discovered;
  }

  if (sc->path.find('*') != std::string::npos) {
    // If the wildcard exists within the file (leaf), remove and monitor the
    // directory instead. Apply a fnmatch on fired events to filter leafs.
    auto fullpath = fs::path(sc->path);
    if (fullpath.filename().string().find('*') != std::string::npos) {
      discovered = fullpath.parent_path().string() + '/';
    }

    if (discovered.find('*') != std::string::npos) {
      // If a wildcard exists within the tree (stem), resolve at configure
      // time and monitor each path.
      std::vector<std::string> paths;
      resolveFilePattern(discovered, paths);
      sc->recursive_match = sc->recursive;
      for (const auto& _path : paths) {
        needMonitoring(_path, sc, sc->mask, sc->recursive, add_watch);
      }
      return true;
    }
  }

  if (isDirectory(discovered) && discovered.back() != '/') {
    sc->path += '/';
    discovered += '/';
  }

  return needMonitoring(discovered, sc, sc->mask, sc->recursive, add_watch);
}

void INotifyEventPublisher::buildExcludePathsSet() {
  auto parser = Config::getParser("file_paths");

  WriteLock lock(subscription_lock_);
  exclude_paths_.clear();
  for (const auto& excl_category :
       parser->getData().get_child("exclude_paths")) {
    for (const auto& excl_path : excl_category.second) {
      auto pattern = excl_path.second.get_value<std::string>("");
      if (pattern.empty()) {
        continue;
      }
      exclude_paths_.insert(pattern);
    }
  }
}

void INotifyEventPublisher::configure() {
  if (inotify_handle_ == -1) {
    // This publisher has not been setup correctly.
    return;
  }

  SubscriptionVector delete_subscriptions;
  {
    WriteLock lock(subscription_lock_);
    auto end = std::remove_if(
        subscriptions_.begin(),
        subscriptions_.end(),
        [&delete_subscriptions](const SubscriptionRef& subscription) {
          auto inotify_sc = getSubscriptionContext(subscription->context);
          if (inotify_sc->mark_for_deletion == true) {
            delete_subscriptions.push_back(subscription);
            return true;
          }
          return false;
        });
    subscriptions_.erase(end, subscriptions_.end());
  }

  for (auto& sub : delete_subscriptions) {
    auto ino_sc = getSubscriptionContext(sub->context);
    for (const auto& path : ino_sc->descriptor_paths_) {
      removeMonitor(path.first, true, true);
    }
    ino_sc->descriptor_paths_.clear();
  }
  delete_subscriptions.clear();

  buildExcludePathsSet();

  for (auto& sub : subscriptions_) {
    // Anytime a configure is called, try to monitor all subscriptions.
    // Configure is called as a response to removing/adding subscriptions.
    // This means recalculating all monitored paths.
    auto sc = getSubscriptionContext(sub->context);
    monitorSubscription(sc);
  }
}

void INotifyEventPublisher::tearDown() {
  if (inotify_handle_ > -1) {
    ::close(inotify_handle_);
  }
  inotify_handle_ = -1;

  WriteLock lock(scratch_mutex_);
  if (scratch_ != nullptr) {
    free(scratch_);
    scratch_ = nullptr;
  }
}

void INotifyEventPublisher::handleOverflow() {
  if (inotify_events_ < kINotifyMaxEvents) {
    VLOG(1) << "inotify was overflown: increasing scratch buffer";
    // Exponential increment.
    inotify_events_ = inotify_events_ * 2;
  } else if (last_overflow_ != -1 && getUnixTime() - last_overflow_ < 60) {
    return;
  } else {
    VLOG(1) << "inotify was overflown";
    last_overflow_ = getUnixTime();
  }
}

Status INotifyEventPublisher::run() {
  struct pollfd fds[1];
  fds[0].fd = getHandle();
  fds[0].events = POLLIN;
  int selector = ::poll(fds, 1, 1000);
  if (selector == -1) {
    if (errno == EINTR) {
      return Status(0, "inotify poll interrupted");
    }
    LOG(WARNING) << "Could not read inotify handle";
    return Status(1, "inotify poll failed");
  }

  if (selector == 0) {
    // Read timeout.
    return Status(0, "Continue");
  }

  if (!(fds[0].revents & POLLIN)) {
    return Status(0, "Invalid poll response");
  }

  WriteLock lock(scratch_mutex_);
  ssize_t record_num =
      ::read(getHandle(), scratch_, inotify_events_ * kINotifyEventSize);
  if (record_num == 0 || record_num == -1) {
    return Status(1, "INotify read failed");
  }

  for (char* p = scratch_; p < scratch_ + record_num;) {
    // Cast the inotify struct, make shared pointer, and append to contexts.
    auto event = reinterpret_cast<struct inotify_event*>(p);
    if (event->mask & IN_Q_OVERFLOW) {
      // The inotify queue was overflown (try to recieve more events from OS).
      handleOverflow();
    } else if (event->mask & IN_IGNORED) {
      // This inotify watch was removed.
      removeMonitor(event->wd, false);
    } else if (event->mask & IN_MOVE_SELF) {
      // This inotify path was moved, but is still watched.
      removeMonitor(event->wd, true);
    } else if (event->mask & IN_DELETE_SELF) {
      // A file was moved to replace the watched path.
      removeMonitor(event->wd, false);
    } else {
      auto ec = createEventContextFrom(event);
      if (!ec->action.empty()) {
        fire(ec);
      }
    }
    // Continue to iterate
    p += (sizeof(struct inotify_event)) + event->len;
  }

  return Status(0, "OK");
}

INotifyEventContextRef INotifyEventPublisher::createEventContextFrom(
    struct inotify_event* event) const {
  auto shared_event = std::make_shared<struct inotify_event>(*event);
  auto ec = createEventContext();
  ec->event = shared_event;

  // Get the pathname the watch fired on.
  {
    WriteLock lock(path_mutex_);
    if (descriptor_inosubctx_.find(event->wd) == descriptor_inosubctx_.end()) {
      // return a blank event context if we can't find the paths for the event
      return ec;
    } else {
      auto isc = descriptor_inosubctx_.at(event->wd);
      ec->path = isc->descriptor_paths_.at(event->wd);
      ec->isub_ctx = isc;
    }
  }

  if (event->len > 1) {
    ec->path += event->name;
  }

  for (const auto& action : kMaskActions) {
    if (event->mask & action.first) {
      ec->action = action.second;
      break;
    }
  }
  return ec;
}

bool INotifyEventPublisher::shouldFire(const INotifySubscriptionContextRef& sc,
                                       const INotifyEventContextRef& ec) const {
  if (sc.get() != ec->isub_ctx.get()) {
    /// Not my event.
    return false;
  }

  // The subscription may supply a required event mask.
  if (sc->mask != 0 && !(ec->event->mask & sc->mask)) {
    return false;
  }

  // inotify will not monitor recursively, new directories need watches.
  if (sc->recursive && ec->action == "CREATED" && isDirectory(ec->path)) {
    const_cast<INotifyEventPublisher*>(this)->addMonitor(
        ec->path + '/',
        const_cast<INotifySubscriptionContextRef&>(sc),
        sc->mask,
        true);
  }

  // exclude paths should be applied at last
  auto path = ec->path.substr(0, ec->path.rfind('/'));
  // Need to have two finds,
  // what if somebody excluded an individual file inside a directory
  if (!exclude_paths_.empty() &&
      (exclude_paths_.find(path) || exclude_paths_.find(ec->path))) {
    return false;
  }

  return true;
}

bool INotifyEventPublisher::addMonitor(const std::string& path,
                                       INotifySubscriptionContextRef& isc,
                                       uint32_t mask,
                                       bool recursive,
                                       bool add_watch) {
  {
    WriteLock lock(path_mutex_);
    int watch = ::inotify_add_watch(
        getHandle(), path.c_str(), ((mask == 0) ? kFileDefaultMasks : mask));
    if (add_watch && watch == -1) {
      LOG(WARNING) << "Could not add inotify watch on: " << path;
      return false;
    }

    if (descriptor_inosubctx_.find(watch) != descriptor_inosubctx_.end()) {
      auto ino_sc = descriptor_inosubctx_.at(watch);
      if (inotify_sanity_check) {
        std::string watched_path = ino_sc->descriptor_paths_[watch];
        path_descriptors_.erase(watched_path);
      }
      ino_sc->descriptor_paths_.erase(watch);
      descriptor_inosubctx_.erase(watch);
    }

    // Keep a map of (descriptor -> path)
    isc->descriptor_paths_[watch] = path;
    descriptor_inosubctx_[watch] = isc;
    if (inotify_sanity_check) {
      // Keep a map of the path -> watch descriptor
      path_descriptors_[path] = watch;
    }
  }

  if (recursive && isDirectory(path).ok()) {
    std::vector<std::string> children;
    // Get a list of children of this directory (requested recursive watches).
    listDirectoriesInDirectory(path, children, true);

    boost::system::error_code ec;
    for (const auto& child : children) {
      auto canonicalized = fs::canonical(child, ec).string() + '/';
      addMonitor(canonicalized, isc, mask, false);
    }
  }

  return true;
}

bool INotifyEventPublisher::removeMonitor(int watch,
                                          bool force,
                                          bool batch_del) {
  {
    WriteLock lock(path_mutex_);
    if (descriptor_inosubctx_.find(watch) == descriptor_inosubctx_.end()) {
      return false;
    }

    auto isc = descriptor_inosubctx_.at(watch);
    descriptor_inosubctx_.erase(watch);

    if (inotify_sanity_check) {
      std::string watched_path = isc->descriptor_paths_[watch];
      path_descriptors_.erase(watched_path);
    }

    if (!batch_del) {
      isc->descriptor_paths_.erase(watch);
    }
  }

  if (force) {
    ::inotify_rm_watch(getHandle(), watch);
  }

  return true;
}

void INotifyEventPublisher::removeSubscriptions(const std::string& subscriber) {
  WriteLock lock(subscription_lock_);
  std::for_each(subscriptions_.begin(),
                subscriptions_.end(),
                [&subscriber](const SubscriptionRef& sub) {
                  if (sub->subscriber_name == subscriber) {
                    getSubscriptionContext(sub->context)->mark_for_deletion =
                        true;
                  }
                });
}

Status INotifyEventPublisher::addSubscription(
    const SubscriptionRef& subscription) {
  WriteLock lock(subscription_lock_);
  auto received_inotify_sc = getSubscriptionContext(subscription->context);
  for (auto& sub : subscriptions_) {
    auto inotify_sc = getSubscriptionContext(sub->context);
    if (*received_inotify_sc == *inotify_sc) {
      if (inotify_sc->mark_for_deletion) {
        inotify_sc->mark_for_deletion = false;
        return Status(0);
      }
      // Returing non zero signals EventSubscriber::subscribe
      // dont bumpup subscription_count_.
      return Status(1);
    }
  }

  subscriptions_.push_back(subscription);
  return Status(0);
}

bool INotifyEventPublisher::isPathMonitored(const std::string& path) const {
  WriteLock lock(path_mutex_);
  std::string parent_path;
  if (!isDirectory(path).ok()) {
    if (path_descriptors_.find(path) != path_descriptors_.end()) {
      // Path is a file, and is directly monitored.
      return true;
    }
    // Important to add a trailing "/" for inotify.
    parent_path = fs::path(path).parent_path().string() + '/';
  } else {
    parent_path = path;
  }
  // Directory or parent of file monitoring
  auto path_iterator = path_descriptors_.find(parent_path);
  return (path_iterator != path_descriptors_.end());
}
}
