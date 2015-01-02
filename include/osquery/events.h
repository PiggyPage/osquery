/*
 *  Copyright (c) 2014, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant 
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#pragma once

#include <functional>
#include <memory>
#include <map>
#include <vector>

#include <boost/make_shared.hpp>
#include <boost/thread.hpp>
#include <boost/thread/locks.hpp>
#include <boost/thread/mutex.hpp>

#include <osquery/database.h>
#include <osquery/registry.h>
#include <osquery/status.h>
#include <osquery/tables.h>

namespace osquery {

struct Subscription;
template <class SC, class EC> class EventPublisher;
template <class PUB> class EventSubscriber;
class EventFactory;

typedef const std::string EventPublisherID;
typedef const std::string EventSubscriberID;
typedef const std::string EventID;
typedef uint32_t EventContextID;
typedef uint32_t EventTime;
typedef std::pair<EventID, EventTime> EventRecord;

/**
 * @brief An EventPublisher will define a SubscriptionContext for
 * EventSubscriber%s to use.
 *
 * Most EventPublisher%s will reqire specific information for interacting with
 * an OS to receive events. The SubscriptionContext contains information the
 * EventPublisher will use to register OS API callbacks, create
 * subscriptioning/listening handles, etc.
 *
 * Linux `inotify` should implement a SubscriptionContext that subscriptions
 * filesystem events based on a filesystem path. `libpcap` will subscribe on
 * networking protocols at various stacks. Process creation may subscribe on
 * process name, parent pid, etc.
 */
struct SubscriptionContext {};

/**
 * @brief An EventSubscriber EventCallback method will receive an EventContext.
 *
 * The EventContext contains the event-related data supplied by an
 * EventPublisher when the event occures. If a subscribing EventSubscriber
 * would be called for the event, the EventSubscriber%'s EventCallback is
 * passed an EventContext.
 */
struct EventContext {
  /// An unique counting ID specific to the EventPublisher%'s fired events.
  EventContextID id;
  /// The time the event occurred.
  EventTime time;
  /// The string representation of the time, often used for indexing.
  std::string time_string;
};

typedef std::shared_ptr<Subscription> SubscriptionRef;
typedef EventPublisher<SubscriptionContext, EventContext> BaseEventPublisher;
typedef std::shared_ptr<BaseEventPublisher> EventPublisherRef;
typedef std::shared_ptr<SubscriptionContext> SubscriptionContextRef;
typedef std::shared_ptr<EventContext> EventContextRef;
typedef std::shared_ptr<EventSubscriber<BaseEventPublisher>> EventSubscriberRef;

/// Use a single placeholder for the EventContextRef passed to EventCallback.
using std::placeholders::_1;
typedef std::function<Status(const EventContextRef&)> EventCallback;

/// An EventPublisher must track every subscription added.
typedef std::vector<SubscriptionRef> SubscriptionVector;

/// The set of search-time binned lookup tables.
extern const std::vector<size_t> kEventTimeLists;

/**
 * @brief DECLARE_PUBLISHER supplies needed boilerplate code that applies a
 * string-type EventPublisherID to identify the publisher declaration.
 */
#define DECLARE_PUBLISHER(TYPE) \
 public:                        \
  EventPublisherID type() { return TYPE; }

/**
 * @brief DECLARE_SUBSCRIBER supplies needed boilerplate code that applies a
 * string-type EventSubscriberID to identify the subscriber declaration.
 */
#define DECLARE_SUBSCRIBER(NAME) \
 public:                         \
  EventSubscriberID name() { return NAME; }

/**
 * @brief A Subscription is used to configure an EventPublisher and bind a
 * callback to a SubscriptionContext.
 *
 * A Subscription is the input to an EventPublisher when the EventPublisher
 * decides on the scope and details of the events it watches/generates.
 * An example includes a filesystem change event. A subscription would include
 * a path with optional recursion and attribute selectors as well as a callback
 * function to fire when an event for that path and selector occurs.
 *
 * A Subscription also functions to greatly scope an EventPublisher%'s work.
 * Using the same filesystem example and the Linux inotify subsystem a
 * Subscription limits the number of inode watches to only those requested by
 * appropriate EventSubscriber%s.
 * Note: EventSubscriber%s and Subscriptions can be configured by the osquery
 * user.
 *
 * Subscriptions are usually created with EventFactory members:
 *
 * @code{.cpp}
 *   EventFactory::addSubscription("MyEventPublisher", my_subscription_context);
 * @endcode
 */
struct Subscription {
 public:
  /// An EventPublisher%-specific SubscriptionContext.
  SubscriptionContextRef context;
  /// An EventSubscription member EventCallback method.
  EventCallback callback;

  static SubscriptionRef create() { return std::make_shared<Subscription>(); }

  static SubscriptionRef create(const SubscriptionContextRef& mc,
                                EventCallback ec = 0) {
    auto subscription = std::make_shared<Subscription>();
    subscription->context = mc;
    subscription->callback = ec;
    return subscription;
  }
};

class EventPublisherCore {
 public:
  /**
   * @brief A new Subscription was added, potentially change state based on all
   * subscriptions for this EventPublisher.
   *
   * `configure` allows the EventPublisher to optimize on the state of all
   * subscriptions. An example is Linux `inotify` where multiple
   * EventSubscription%s will subscription identical paths, e.g., /etc for
   * config changes. Since Linux `inotify` has a subscription limit, `configure`
   * can depup paths.
   */
  virtual void configure() {}

  /**
   * @brief Perform handle opening, OS API callback registration.
   *
   * `setUp` is the event framework's EventPublisher constructor equivilent.
   * When `setUp` is called the EventPublisher is running in a dedicated thread
   * and may manage/allocate/wait for resources.
   */
  virtual Status setUp() { return Status(0, "Not used"); }

  /**
   * @brief Perform handle closing, resource cleanup.
   *
   * osquery is about to end, the EventPublisher should close handle descriptors
   * unblock resources, and prepare to exit.
   */
  virtual void tearDown() {}

  /**
   * @brief Implement a step of an optional run loop.
   *
   * @return A SUCCESS status will immediately call `run` again. A FAILED status
   * will exit the run loop and the thread.
   */
  virtual Status run() { return Status(1, "No runloop required"); }

  /**
   * @brief A new EventSubscriber is subscriptioning events of this
   * EventPublisher.
   *
   * @param subscription The Subscription context information and optional
   * EventCallback.
   *
   * @return If the Subscription is not appropriate (mismatched type) fail.
   */
  virtual Status addSubscription(const SubscriptionRef& subscription) {
    subscriptions_.push_back(subscription);
    return Status(0, "OK");
  }

  /**
   * @brief The generic check loop to call SubscriptionContext callback methods.
   *
   * It is NOT recommended to override `fire`. The simple logic of enumerating
   * the Subscription%s and using `shouldFire` is more appropraite.
   *
   * @param ec The EventContext created and fired by the EventPublisher.
   * @param time The most accurate time associated with the event.
   */
  void fire(const EventContextRef& ec, EventTime time = 0);

  /// Number of Subscription%s watching this EventPublisher.
  size_t numSubscriptions() { return subscriptions_.size(); }

  /**
   * @brief The number of events fired by this EventPublisher.
   *
   * @return The number of events.
   */
  size_t numEvents() { return next_ec_id_; }

  /// Overriding the EventPublisher constructor is not recommended.
  EventPublisherCore() : next_ec_id_(0){};
  virtual ~EventPublisherCore() {}

  /// Return a string identifier associated with this EventPublisher.
  virtual EventPublisherID type() { return "publisher"; }

 protected:
  /// The internal fire method used by the typed EventPublisher.
  virtual void fireCallback(const SubscriptionRef& sub,
                            const EventContextRef& ec) = 0;

  /// The EventPublisher will keep track of Subscription%s that contain callins.
  SubscriptionVector subscriptions_;

  /// An Event ID is assigned by the EventPublisher within the EventContext.
  /// This is not used to store event date in the backing store.
  EventContextID next_ec_id_;

 private:
  /// A lock for incrementing the next EventContextID.
  boost::mutex ec_id_lock_;

 private:
  FRIEND_TEST(EventsTests, test_event_pub);
  FRIEND_TEST(EventsTests, test_fire_event);
};

/**
 * @brief Generate OS events of a type (FS, Network, Syscall, ioctl).
 *
 * A 'class' of OS events is abstracted into an EventPublisher responsible for
 * remaining as agile as possible given a known-set of subscriptions.
 *
 * The lifecycle of an EventPublisher may include, `setUp`, `configure`, `run`,
 * `tearDown`, and `fire`. `setUp` and `tearDown` happen when osquery starts and
 * stops either as a daemon or interactive shell. `configure` is a pseudo-start
 * called every time a Subscription is added. EventPublisher%s can adjust their
 * scope/agility specific to each added subscription by overriding
 *`addSubscription`, and/or globally in `configure`.
 *
 * Not all EventPublisher%s leverage pure async OS APIs, and most will require a
 * run loop either polling with a timeout on a descriptor or for a change. When
 * osquery initializes the EventFactory will optionally create a thread for each
 * EventPublisher using `run` as the thread's entrypoint. `run` is called in a
 * within-thread loop where returning a FAILED status ends the run loop and
 * shuts down the thread.
 *
 * To opt-out of polling in a thread, consider the following run implementation:
 *
 * @code{.cpp}
 *   Status run() { return Status(1, "Not Implemented"); }
 * @endcode
 *
 * The final lifecycle component, `fire` will iterate over the EventPublisher
 * Subscription%s and call `shouldFire` for each, using the EventContext fired.
 * The `shouldFire` method should check the subscription-specific selectors and
 * only call the Subscription%'s callback function if the EventContext
 * (thus event) matches.
 */
template <typename SC, typename EC>
class EventPublisher : public EventPublisherCore {
 public:
  /// A nested helper typename for the templated SubscriptionContextRef.
  typedef typename std::shared_ptr<SC> SCRef;
  /// A nested helper typename for the templated EventContextRef.
  typedef typename std::shared_ptr<EC> ECRef;

 public:
  /// Up-cast a base EventContext reference to the templated ECRef.
  static ECRef getEventContext(const EventContextRef& ec) {
    return std::static_pointer_cast<EC>(ec);
  }

  /// Up-cast a base SubscriptionContext reference to the templated SCRef.
  static SCRef getSubscriptionContext(const SubscriptionContextRef& sc) {
    return std::static_pointer_cast<SC>(sc);
  }

  /// Create a EventContext based on the templated type.
  static ECRef createEventContext() { return std::make_shared<EC>(); }

  /// Create a SubscriptionContext based on the templated type.
  static SCRef createSubscriptionContext() { return std::make_shared<SC>(); }

  /// A simple EventPublisher type accessor.
  template <class PUB>
  static EventPublisherID getType() {
    auto pub = std::make_shared<PUB>();
    return pub->type();
  }

 protected:
  /**
   * @brief The internal `fire` phase of publishing.
   *
   * This is a template-generated method that up-casts the generic fired
   * event/subscription contexts, and calls the callback if the event should
   * fire given a scription.
   *
   * @param sub The SubscriptionContext and optional EventCallback.
   * @param ec The event that was fired.
   */
  void fireCallback(const SubscriptionRef& sub, const EventContextRef& ec) {
    auto pub_sc = getSubscriptionContext(sub->context);
    auto pub_ec = getEventContext(ec);
    if (shouldFire(pub_sc, pub_ec) && sub->callback != nullptr) {
      sub->callback(pub_ec);
    }
  }

 protected:
  /**
   * @brief The generic `fire` will call `shouldFire` for each Subscription.
   *
   * @param mc A SubscriptionContext with optional specifications for events
   * details.
   * @param ec The event fired with event details.
   *
   * @return should the Subscription%'s EventCallback be called for this event.
   */
  virtual bool shouldFire(const SCRef sc, const ECRef ec) { return true; }

 private:
  FRIEND_TEST(EventsTests, test_event_sub_subscribe);
  FRIEND_TEST(EventsTests, test_event_sub_context);
  FRIEND_TEST(EventsTests, test_fire_event);
};

/**
 * @brief A factory for associating event generators to EventPublisherID%s.
 *
 * This factory both registers new event types and the subscriptions that use
 * them. An EventPublisher is also a factory, the single event factory arbitates
 * Subscription creatating and management for each associated EventPublisher.
 *
 * Since event types may be plugins, they are created using the factory.
 * Since subscriptions may be configured/disabled they are also factory-managed.
 */
class EventFactory {
 public:
  /// Access to the EventFactory instance.
  static EventFactory& getInstance();

  /// A factory event publisher generator, simplify boilerplate code.
  template <class PUB>
  static EventPublisherRef createEventPublisher() {
    auto pub = std::make_shared<PUB>();
    auto base_pub = reinterpret_cast<EventPublisherRef&>(pub);
    return base_pub;
  }

  /// A factory event subscriber generator, simplify boilerplate code.
  template <class SUB>
  static EventSubscriberRef createEventSubscriber() {
    auto sub = std::make_shared<SUB>();
    auto base_sub = reinterpret_cast<EventSubscriberRef&>(sub);
    return base_sub;
  }

  /**
   * @brief Add an EventPublisher to the factory.
   *
   * The registration is mostly abstracted using osquery's registery.
   */
  template <class T>
  static Status registerEventPublisher() {
    auto pub = std::make_shared<T>();
    return registerEventPublisher(pub);
  }

  /**
   * @brief Add an EventPublisher to the factory.
   *
   * The registration is mostly abstracted using osquery's registery.
   *
   * @param event_pub If for some reason the caller needs access to the
   * EventPublisher instance they can register-by-instance.
   *
   * Access to the EventPublisher instance is not discouraged, but using the
   * EventFactory `getEventPublisher` accessor is encouraged.
   */
  template <class T>
  static Status registerEventPublisher(std::shared_ptr<T> pub) {
    auto base_pub = reinterpret_cast<EventPublisherRef&>(pub);
    return registerEventPublisher(base_pub);
  }

  /// Once the event publisher has been down-casted, call it's API.
  static Status registerEventPublisher(const EventPublisherRef& pub);

  /**
   * @brief Add an EventSubscriber to the factory.
   *
   * The registration is mostly abstracted using osquery's registery.
   */
  template <class T>
  static Status registerEventSubscriber() {
    auto sub = std::make_shared<T>();
    return registerEventSubscriber(sub);
  }

  /**
   * @brief Add an EventSubscriber to the factory.
   *
   * The registration is mostly abstracted using osquery's registery.
   *
   * @param sub If the caller must access the EventSubscriber instance
   * control may be passed to the registry.
   *
   * Access to the EventSubscriber instance outside of the within-instance
   * table generation method and set of EventCallback%s is discouraged.
   */
  template <class T>
  static Status registerEventSubscriber(std::shared_ptr<T> sub) {
    auto base_sub = reinterpret_cast<EventSubscriberRef&>(sub);
    return registerEventSubscriber(base_sub);
  }

  /// Once the event subscriber has been down-casted, call it's API.
  static Status registerEventSubscriber(const EventSubscriberRef& sub);

  /**
   * @brief Add a SubscriptionContext and EventCallback Subscription to an
   *EventPublisher.
   *
   * Create a Subscription from a given SubscriptionContext and EventCallback
   * and add that Subscription to the EventPublisher assosicated identiter.
   *
   * @param type_id The string for an EventPublisher receiving the Subscription.
   * @param mc A SubscriptionContext related to the EventPublisher.
   * @param cb When the EventPublisher fires an event the SubscriptionContext
   * will be evaluated, if the event matches optional specifics in the context
   * this callback function will be called. It should belong to an
   * EventSubscription.
   *
   * @return Was the SubscriptionContext appropriate for the EventPublisher.
   */
  static Status addSubscription(EventPublisherID& type_id,
                                const SubscriptionContextRef& mc,
                                EventCallback cb = 0);

  /// Add a Subscription by templating the EventPublisher, using a
  /// SubscriptionContext.
  template <typename PUB>
  static Status addSubscription(const SubscriptionContextRef& mc,
                                EventCallback cb = 0) {
    return addSubscription(BaseEventPublisher::getType<PUB>(), mc, cb);
  }

  /// Add a Subscription using a caller Subscription instance.
  static Status addSubscription(EventPublisherID& type_id,
                                const SubscriptionRef& subscription);

  /// Get the total number of Subscription%s across ALL EventPublisher%s.
  static size_t numSubscriptions(EventPublisherID& type_id);

  /// Get the number of EventPublishers.
  static size_t numEventPublishers() {
    return EventFactory::getInstance().event_pubs_.size();
  }

  /**
   * @brief Halt the EventPublisher run loop and call its `tearDown`.
   *
   * Any EventSubscriber%s with Subscription%s for this EventPublisher will
   * become useless. osquery instanciators MUST deregister events.
   * EventPublisher%s assume they can hook/trampoline, which requires cleanup.
   *
   * @param event_pub The string label for the EventPublisher.
   *
   * @return Did the EventPublisher deregister cleanly.
   */
  static Status deregisterEventPublisher(const EventPublisherRef& pub);

  /// Deregister an EventPublisher by EventPublisherID.
  static Status deregisterEventPublisher(EventPublisherID& type_id);

  /// Deregister all EventPublisher%s.
  static Status deregisterEventPublishers();

  /// Return an instance to a registered EventPublisher.
  static EventPublisherRef getEventPublisher(EventPublisherID& pub);

  /// Return an instance to a registered EventSubscriber.
  static EventSubscriberRef getEventSubscriber(EventSubscriberID& pub);

 public:
  /// The dispatched event thread's entrypoint (if needed).
  static Status run(EventPublisherID& type_id);

  /// An initializer's entrypoint for spawning all event type run loops.
  static void delay();

 public:
  /// If a static EventPublisher callback wants to fire
  template <typename PUB>
  static void fire(const EventContextRef& ec) {
    auto event_pub = getEventPublisher(BaseEventPublisher::getType<PUB>());
    event_pub->fire(ec);
  }

  /**
   * @brief End all EventPublisher run loops and call their `tearDown` methods.
   *
   * End is NOT the same as deregistration.
   *
   * @param should_end Reset the "is ending" state if False.
   */
  static void end(bool should_end = true);

 private:
  /// An EventFactory will exist for the lifetime of the application.
  EventFactory() { ending_ = false; }
  EventFactory(EventFactory const&);
  void operator=(EventFactory const&);

 private:
  /// Set ending to True to cause event type run loops to finish.
  bool ending_;

  /// Set of registered EventPublisher instances.
  std::map<EventPublisherID, EventPublisherRef> event_pubs_;

  /// Set of instanciated EventSubscriber subscriptions.
  std::map<EventSubscriberID, EventSubscriberRef> event_subs_;

  /// Set of running EventPublisher run loop threads.
  std::vector<std::shared_ptr<boost::thread> > threads_;
};

class EventSubscriberCore {
 protected:
  /**
   * @brief Store parsed event data from an EventCallback in a backing store.
   *
   * Within a EventCallback the EventSubscriber has an opprotunity to create
   * an osquery Row element, add the relevant table data for the EventSubscriber
   * and store that element in the osquery backing store. At query-time
   * the added data will apply selection criteria and return these elements.
   * The backing store data retrieval is optimized by time-based indexes. It
   * is important to added EventTime as it relates to "when the event occurred".
   *
   * @param r An osquery Row element.
   * @param time The time the added event occurred.
   *
   * @return Was the element added to the backing store.
   */
  virtual Status add(const osquery::Row& r, EventTime time) final;

  /**
   * @brief Return all events added by this EventSubscriber within start, stop.
   *
   * This is used internally (for the most part) by EventSubscriber::genTable.
   *
   * @param start Inclusive lower bound time limit.
   * @param stop Inclusive upper bound time limit.
   * @return Set of event rows matching time limits.
   */
  virtual QueryData get(EventTime start, EventTime stop);

 private:
  /*
   * @brief When `get`ting event results, return EventID%s from time indexes.
   *
   * Used by EventSubscriber::get to retrieve EventID, EventTime indexes. This
   * applies the lookup-efficiency checks for time list appropriate bins.
   * If the time range in 24 hours and there is a 24-hour list bin it will
   * be queried using a single backing store `Get` followed by two `Get`s of
   * the most-specific boundary lists.
   *
   * @return List of EventID, EventTime%s
   */
  std::vector<EventRecord> getRecords(const std::vector<std::string>& indexes);

  /**
   * @brief Get a unique storage-related EventID.
   *
   * An EventID is an index/element-identifier for the backing store.
   * Each EventPublisher maintains a fired EventContextID to identify the many
   * events that may or may not be fired to subscriptioning criteria for this
   * EventSubscriber. This EventContextID is NOT the same as an EventID.
   * EventSubscriber development should not require use of EventID%s, if this
   * indexing is required within-EventCallback consider an
   * EventSubscriber%-unique indexing, counting mechanic.
   *
   * @return A unique ID for backing storage.
   */
  EventID getEventID();

  /**
   * @brief Plan the best set of indexes for event record access.
   *
   * @param start an inclusive time to begin searching.
   * @param stop an inclusive time to end searching.
   * @param list_key optional key to bind to a specific index binning.
   *
   * @return List of 'index.step' index strings.
   */
  std::vector<std::string> getIndexes(EventTime start, 
                                      EventTime stop,
                                      int list_key = 0);

  /**
   * @brief Expire indexes and eventually records.
   *
   * @param list_type the string representation of list binning type.
   * @param indexes complete set of 'index.step' indexes for the list_type.
   * @param expirations of the indexes, the set to expire.
   *
   * @return status if the indexes and records were removed.
   */
  Status expireIndexes(const std::string& list_type,
                       const std::vector<std::string>& indexes,
                       const std::vector<std::string>& expirations);

  /**
   * @brief Add an EventID, EventTime pair to all matching list types.
   *
   * The list types are defined by time size. Based on the EventTime this pair
   * is added to the list bin for each list type. If there are two list types:
   * 60 seconds and 3600 seconds and `time` is 92, this pair will be added to
   * list type 1 bin 4 and list type 2 bin 1.
   *
   * @param eid A unique EventID.
   * @param time The time when this EventID%'s event occurred.
   *
   * @return Were the indexes recorded.
   */
  Status recordEvent(EventID& eid, EventTime time);

 public:
  /**
   * @brief A single instance requirement for static callback facilities.
   *
   * The EventSubscriber constructor is NOT responsible for adding
   * Subscription%s. Please use `init` for adding Subscription%s as all
   * EventPublisher instances will have run `setUp` and initialized their run
   * loops.
   */
  EventSubscriberCore() {
    expire_events_ = true;
    expire_time_ = 0;
  }
  ~EventSubscriberCore() {}

  /**
   * @brief Suggested entrypoint for table generation.
   *
   * The EventSubscriber is a convention that removes a lot of boilerplate event
   * subscriptioning and acting. The `genTable` static entrypoint is the
   * suggested method for table specs.
   *
   * @return The query-time table data, retrieved from a backing store.
   */
  virtual QueryData genTable(tables::QueryContext& context)
      __attribute__((used)) {
    return get(0, 0);
  }

  /// The string name identifying this EventSubscriber.
  virtual EventSubscriberID name() { return "subscriber"; }

 protected:
  /// Backing storage indexing namespace definition methods.
  EventPublisherID dbNamespace() { return type() + "." + name(); }

  /// The string EventPublisher identifying this EventSubscriber.
  virtual EventPublisherID type() = 0;

  /// Disable event expiration for this subscriber.
  void doNotExpire() { expire_events_ = false; }

 private:
  /// Do not respond to periodic/scheduled/triggered event expiration requests.
  bool expire_events_;

  /// Events before the expire_time_ are invalid and will be purged.
  EventTime expire_time_;

  /// Lock used when incrementing the EventID database index.
  boost::mutex event_id_lock_;

  /// Lock used when recording an EventID and time into search bins.
  boost::mutex event_record_lock_;

 private:
  FRIEND_TEST(EventsDatabaseTests, test_event_module_id);
  FRIEND_TEST(EventsDatabaseTests, test_record_indexing);
  FRIEND_TEST(EventsDatabaseTests, test_record_range);
  FRIEND_TEST(EventsDatabaseTests, test_record_expiration);
};

/**
 * @brief An interface binding Subscriptions, event response, and table
 *generation.
 *
 * Use the EventSubscriber interface when adding event subscriptions and
 * defining callin functions. The EventCallback is usually a member function
 * for an EventSubscriber. The EventSubscriber interface includes a very
 * important `add` method that abstracts the needed event to backing store
 * interaction.
 *
 * Storing event data in the backing store must match a table spec for queries.
 * Small overheads exist that help query-time indexing and lookups.
 */
template <class PUB>
class EventSubscriber: public EventSubscriberCore {
 protected:
  typedef typename PUB::SCRef SCRef;
  typedef typename PUB::ECRef ECRef;

 public:
  /// Called after EventPublisher `setUp`. Add all Subscription%s here.
  /**
   * @brief Add Subscription%s to the EventPublisher this module will act on.
   *
   * When the EventSubscriber%'s `init` method is called you are assured the
   * EventPublisher has `setUp` and is ready to subscription for events.
   */
  virtual void init() {}

  /// Helper function to call the publisher's templated subscription generator.
  SCRef createSubscriptionContext() { return PUB::createSubscriptionContext(); }

  /**
   * @brief Bind a registered EventSubscriber member function to a Subscription.
   *
   * @param entry A templated EventSubscriber member function.
   * @param sc The subscription context.
   */
  template <class T, typename C>
  void subscribe(Status (T::*entry)(const std::shared_ptr<C>&),
                 const SubscriptionContextRef& sc) {
    // Up-cast the CRTP-style EventSubscriber to the caller.
    auto self = dynamic_cast<T*>(this);
    // Down-cast the pointer to the member function.
    auto base_entry =
        reinterpret_cast<Status (T::*)(const EventContextRef&)>(entry);
    // Create a callable to the member function using the instance of the
    // EventSubscriber and a single parameter placeholder (the EventContext).
    auto cb = std::bind(base_entry, self, _1);
    // Add a subscription using the callable and SubscriptionContext.
    EventFactory::addSubscription(type(), sc, cb);
  }

  /// Helper EventPublisher string type accessor.
  EventPublisherID type() { return BaseEventPublisher::getType<PUB>(); }

 private:
  FRIEND_TEST(EventsTests, test_event_sub);
  FRIEND_TEST(EventsTests, test_event_sub_subscribe);
  FRIEND_TEST(EventsTests, test_event_sub_context);
};
}

/// Expose a Plugin-like Registry for EventPublisher instances.
DECLARE_REGISTRY(EventPublishers, std::string, EventPublisherRef);
#define REGISTERED_EVENTPUBLISHERS REGISTRY(EventPublishers)
#define REGISTER_EVENTPUBLISHER(PUB) \
  REGISTER(EventPublishers, #PUB, EventFactory::createEventPublisher<PUB>());

/**
 * @brief Expose a Plugin-link Registry for EventSubscriber instances.
 *
 * In most cases the EventSubscriber class will organize itself to include
 * an generator entry point for query-time table generation too.
 */
DECLARE_REGISTRY(EventSubscribers, std::string, EventSubscriberRef);
#define REGISTERED_EVENTSUBSCRIBERS REGISTRY(EventSubscribers)
#define REGISTER_EVENTSUBSCRIBER(SUB) \
  REGISTER(EventSubscribers, #SUB, EventFactory::createEventSubscriber<SUB>());

namespace osquery {
namespace registries {
/**
 * @brief A utility method for moving EventPublisher%s and EventSubscriber%s
 * (plugins) into the EventFactory.
 *
 * To handle run-time and compile-time EventPublisher and EventSubscriber
 * additions as plugins or extensions, the osquery Registry workflow is used.
 * During application launch (or within plugin load) the EventFactory faucet
 * moves managed instances of these types to the EventFactory. The
 * EventPublisher and EventSubscriber lifecycle/developer workflow is unknown
 * to the Registry.
 */
void faucet(EventPublishers ets, EventSubscribers ems);
}
}
