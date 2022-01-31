#pragma once
#include "RtdManager.h"
#include <xloil/Log.h>

#include <atlbase.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <shared_mutex>
#include <memory>
#include <vector>

using std::vector;
using std::shared_ptr;
using std::scoped_lock;
using std::unique_lock;
using std::shared_lock;
using std::wstring;
using std::unordered_set;
using std::unordered_map;
using std::pair;
using std::list;
using std::atomic;
using std::mutex;

namespace xloil
{
  namespace COM
  {
    template <class TValue>
    class RtdServerThreadedWorker : public IRtdServerWorker, public IRtdPublishManager<TValue>
    {
    public:

      void start(std::function<void()>&& updateNotify)
      {
        _updateNotify = std::move(updateNotify);
        _isRunning = true;
        _workerThread = std::thread([=]() { this->workerThreadMain(); });
      }
      void connect(long topicId, wstring&& topic)
      {
        {
          unique_lock lock(_mutexNewSubscribers);
          _topicsToConnect.emplace_back(topicId, std::move(topic));
        }
        notify();
      }
      void disconnect(long topicId)
      {
        {
          unique_lock lock(_mutexNewSubscribers);
          _topicIdsToDisconnect.emplace_back(topicId);
        }
        notify();
      }

      SAFEARRAY* getUpdates() 
      { 
        auto updates = _readyUpdates.exchange(nullptr);
        notify();
        return updates;
      }

      void quit()
      {
        if (!isServerRunning())
          return; // Already terminated, or never started

        setQuitFlag();
        // Let thread know we have set 'quit' flag
        notify();
      }

      void join()
      {
        quit();
        if (_workerThread.joinable())
          _workerThread.join();
      }

      void update(wstring&& topic, const shared_ptr<TValue>& value)
      {
        if (!isServerRunning())
          return;
        {
          scoped_lock lock(_mutexNewValues);
          // TODO: can this be somehow lock free?
          _newValues.emplace_back(make_pair(std::move(topic), value));
        }
        notify();
      }

      void addPublisher(const shared_ptr<IRtdPublisher>& job)
      {
        auto existingJob = job;
        {
          unique_lock lock(_lockRecords);
          auto& record = _records[job->topic()];
          std::swap(record.publisher, existingJob);
          if (existingJob)
            _cancelledPublishers.push_back(existingJob);
        }
        if (existingJob)
          existingJob->stop();
      }

      bool dropPublisher(const wchar_t* topic)
      {
        // We must not hold the lock when calling functions on the publisher
        // as they may try to call other functions on the RTD server. 
        shared_ptr<IRtdPublisher> publisher;
        {
          unique_lock lock(_lockRecords);
          auto i = _records.find(topic);
          if (i == _records.end())
            return false;
          std::swap(publisher, i->second.publisher);
        }

        // Signal the publisher to stop
        publisher->stop();

        // Destroy producer, the dtor of RtdPublisher waits for completion
        publisher.reset();

        // Publish empty value (which triggers a notify)
        update(topic, shared_ptr<TValue>());
        return true;
      }

      bool value(const wchar_t* topic, shared_ptr<const TValue>& val) const
      {
        shared_lock lock(_lockRecords);
        auto found = _records.find(topic);
        if (found == _records.end())
          return false;

        val = found->second.value;
        return true;
      }

    private:
      unordered_map<long, wstring> _activeTopicIds;

      struct TopicRecord
      {
        shared_ptr<IRtdPublisher> publisher;
        unordered_set<long> subscribers;
        shared_ptr<TValue> value;
      };

      unordered_map<wstring, TopicRecord> _records;

      list<pair<wstring, shared_ptr<TValue>>> _newValues;
      vector<pair<long, wstring>> _topicsToConnect;
      vector<long> _topicIdsToDisconnect;

      // Publishers which have been cancelled but haven't finished terminating
      list<shared_ptr<IRtdPublisher>> _cancelledPublishers;

      std::function<void()> _updateNotify;
      atomic<SAFEARRAY*> _readyUpdates;
      atomic<bool> _isRunning;

      // We use a separate lock for the newValues to avoid blocking too 
      // often: value updates are likely to come from other threads and 
      // simply need to write into newValues without accessing pub/sub info.
      // We use _lockRecords for all other synchronisation
      mutable mutex _mutexNewValues;
      mutable mutex _mutexNewSubscribers;
      mutable std::shared_mutex _lockRecords;

      std::thread _workerThread;
      std::condition_variable _workPendingNotifier;
      atomic<bool> _workPending = false;

      void notify() noexcept
      {
        _workPending = true;
        _workPendingNotifier.notify_one();
      }

      void setQuitFlag()
      {
        _isRunning = false;
      }

      bool isServerRunning() const
      {
        return _isRunning;
      }

      void workerThreadMain()
      {
        unordered_set<long> readyTopicIds;

        while (isServerRunning())
        {
          // The worker does all the work!  In this order
          //   1) Wait for wake notification
          //   2) Check if quit/stop has been sent
          //   3) Look for new values.
          //      a) If any, put the matching topicIds in readyTopicIds
          //      b) If Excel has picked up previous values, create an array of 
          //         updates and send an UpdateNotify.
          //   4) Run any topic connect requests
          //   5) Run any topic disconnect requests
          //   6) Repeat
          //
          decltype(_newValues) newValues;

          unique_lock lockValues(_mutexNewValues);
          // This slightly convoluted code protects against spurious wakes and 
          // 'lost' wakes, i.e. if the CV is signalled but the worker is not
          // in the waiting state.
          if (!_workPending)
            _workPendingNotifier.wait(lockValues, [&]() { return _workPending.load(); });
          _workPending = false;

          if (!isServerRunning())
            break;

          // Since _mutexNewValues is required to send updates, so we avoid holding it  
          // and quickly swap out the list of new values. 
          std::swap(newValues, _newValues);
          lockValues.unlock();

          if (!newValues.empty())
          {
            shared_lock lock(_lockRecords);
            auto iValue = newValues.begin();
            for (; iValue != newValues.end(); ++iValue)
            {
              auto record = _records.find(iValue->first);
              if (record == _records.end())
                continue;
              record->second.value = iValue->second;
              readyTopicIds.insert(record->second.subscribers.begin(), record->second.subscribers.end());
            }
          }

          // When RefreshData runs, it will take the SAFEARRAY in _readyUpdates and
          // atomically replace it with null. So if this ptr is not null, we know Excel
          // has not yet picked up the new values.
          if (!readyTopicIds.empty() && !_readyUpdates)
          {
            const auto nReady = readyTopicIds.size();

            SAFEARRAYBOUND bounds[] = { { 2u, 0 }, { (ULONG)nReady, 0 } };
            auto* topicArray = SafeArrayCreate(VT_VARIANT, 2, bounds);
            writeReadyTopicsArray(topicArray, readyTopicIds);
            _readyUpdates = topicArray;

            _updateNotify();

            readyTopicIds.clear();
          }

          decltype(_topicsToConnect) topicsToConnect;
          decltype(_topicIdsToDisconnect) topicIdsToDisconnect;
          {
            unique_lock lock(_mutexNewSubscribers);

            std::swap(_topicIdsToDisconnect, topicIdsToDisconnect);
            std::swap(_topicsToConnect, topicsToConnect);
          }
          for (auto& [topicId, topic] : topicsToConnect)
            connectTopic(topicId, std::move(topic));
          for (auto topicId : topicIdsToDisconnect)
            disconnectTopic(topicId);
        }

        // Clear all records, destroy all publishers
        clear();
      }

      // 
      // Creates a 2 x n safearray which has rows of:
      //     topicId | empty
      // With the topicId for each updated topic. The second column can be used
      // to pass an updated value to Excel, however, only string values are allowed
      // which is too restricive. Passing empty tells Excel to call the function
      // again to get the value
      //
      void writeReadyTopicsArray(
        SAFEARRAY* data,
        const std::unordered_set<long>& topics,
        const long startRow = 0)
      {
        void* element = nullptr;
        auto iRow = startRow;
        for (auto topic : topics)
        {
          long index[] = { 0, iRow };
          auto ret = SafeArrayPtrOfIndex(data, index, &element);
          assert(S_OK == ret);
          *(VARIANT*)element = _variant_t(topic);

          index[0] = 1;
          ret = SafeArrayPtrOfIndex(data, index, &element);
          assert(S_OK == ret);
          *(VARIANT*)element = _variant_t();

          ++iRow;
        }
      }

      void connectTopic(long topicId, wstring&& topic)
      {
        XLO_TRACE(L"RTD: connect '{}' to topicId '{}'", topic, topicId);

        // We need these values after we release the lock
        shared_ptr<IRtdPublisher> publisher;
        size_t numSubscribers;

        {
          unique_lock lock(_lockRecords);

          // Find subscribers for this topic and link to the topic ID
          auto& record = _records[topic];
          record.subscribers.insert(topicId);
          publisher = record.publisher;
          numSubscribers = record.subscribers.size();

          _activeTopicIds.emplace(topicId, std::move(topic));
        }

        // Let the publisher know how many subscribers they now have.
        // We must not hold the lock when calling functions on the publisher
        // as they may try to call other functions on the RTD server. 
        if (publisher)
          publisher->connect(numSubscribers);
      }

      void disconnectTopic(long topicId)
      {
        shared_ptr<IRtdPublisher> publisher;
        size_t numSubscribers;
        decltype(_cancelledPublishers) cancelledPublishers;

        // We must *not* hold the lock when calling methods of the publisher
        // as they may try to call other functions on the RTD server. So we
        // first handle the topic lookup and removing subscribers before
        // releasing the lock and notifying the publisher.
        {
          unique_lock lock(_lockRecords);

          XLO_TRACE("RTD: disconnect topicId {}", topicId);

          std::swap(_cancelledPublishers, cancelledPublishers);

          const auto topic = _activeTopicIds.find(topicId);
          if (topic == _activeTopicIds.end())
            XLO_THROW("Could not find topic for id {0}", topicId);

          auto& record = _records[topic->second];
          record.subscribers.erase(topicId);

          numSubscribers = record.subscribers.size();
          publisher = record.publisher;

          if (!publisher && numSubscribers == 0)
            _records.erase(topic->second);

          _activeTopicIds.erase(topic);
        }

        // Remove any cancelled publishers which have finalised
        cancelledPublishers.remove_if([](auto& x) { return x->done(); });

        if (!publisher)
          return;

        // If disconnect() causes the publisher to cancel its task,
        // it will return true here. We may not be able to just delete it: 
        // we have to wait until any threads it created have exited
        if (publisher->disconnect(numSubscribers))
        {
          const auto topic = publisher->topic();
          const auto done = publisher->done();

          if (!done)
            publisher->stop();
          {
            unique_lock lock(_lockRecords);

            // Not done, so add to this list and check back later
            if (!done)
              _cancelledPublishers.emplace_back(publisher);

            // Disconnect should only return true when num_subscribers = 0, 
            // so it's safe to erase the entire record
            _records.erase(topic);
          }
        }
      }

      void clear()
      {
        // We must not hold any locks when calling functions on the publisher
        // as they may try to call other functions on the RTD server. 
        list<shared_ptr<IRtdPublisher>> publishers;
        {
          unique_lock lock(_lockRecords);

          for (auto& record : _records)
            if (record.second.publisher)
              publishers.emplace_back(std::move(record.second.publisher));

          _records.clear();
          _cancelledPublishers.clear();
        }

        for (auto& pub : publishers)
        {
          try
          {
            pub->stop();
          }
          catch (const std::exception& e)
          {
            XLO_INFO(L"Failed to stop producer: '{0}': {1}",
              pub->topic(), utf8ToUtf16(e.what()));
          }
        }
      }
    };
  }
}