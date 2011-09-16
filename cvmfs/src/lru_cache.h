/**
 * \file lru_cache.h
 * \namespace cvmfs
 *
 * This class provides an Least Recently Used (LRU) cache for arbitrary data
 * It stores Key-Value pairs of arbitrary data types in a fast hash which automatically
 * deletes the entries which are least touched in the last time to maintain a given
 * maximal cache size.
 *
 * usage:
 *   LruCache<int, string> cache(100);  // cache mapping ints to strings with maximal size of 100
 *
 *   // inserting some stuff
 *   cache.insert(42, "fourtytwo");
 *   cache.insert(2, "small prime number");
 *   cache.insert(1337, "leet");
 *
 *   // trying to retrieve a value
 *   int result;
 *   if (cache.lookup(21, result)) {
 *      cout << "cache hit: " << result << endl;
 *   } else {
 *      cout << "cache miss" << endl;
 *   }
 *
 *   // maintaining the cache
 *   cache.resize(2);  // changing maximal size to '2' dropping entries to fit new max size
 *   cache.drop();     // empty the cache
 *
 * Developed by René Meusel 2011 at CERN
 * rene@renemeusel.de
 */

#ifndef LRU_CACHE_H
#define LRU_CACHE_H 1

#include <assert.h>
#include <map>
#include <iostream>

namespace cvmfs {

	/**
	 *  template class to create a LRU cache
	 *  @param Key type of the key values
	 *  @param Value type of the value values
	 */
	template<class Key, class Value>
	class LruCache {
		
	private:
		// forward declarations of private internal data structures
		template<class T> class ListEntry;
		template<class T> class ListEntryHead;
		template<class T> class ListEntryContent;
	
		typedef struct {
			ListEntryContent<Key> *listEntry;
			Value value;
		} CacheEntry;
	
		typedef std::map<Key, CacheEntry> Cache;

		// internal data fields
		unsigned int mCurrentCacheSize;
		unsigned int mMaxCacheSize;
		
		/**
		 *  a double linked list to keep track of the least recently
		 *  used data entries.
		 *  New entries get pushed back to the list. If an entry is touched
		 *  it is moved to the back of the list again.
		 *  If the cache gets too long, the first element (the oldest) gets
		 *  deleted to obtain some space.
		 */
		ListEntryHead<Key> *mLruList;
		
		/**
		 *  the actual caching data structure
		 *  it has to be a map of some kind... lookup performance is crucial
		 */
		Cache mCache;
	
		/**
		 *  INTERNAL DATA STRUCTURE
		 *  abstract ListEntry class to maintain a double linked list
		 *  the list keeps track of the least recently used keys in the cache
		 */
		template<class T>
		class ListEntry {
		public:
			ListEntry<T> *next; // <-- pointer to next element in the list
			ListEntry<T> *prev; // <-- ... take an educated guess... ;-)

		public:
			ListEntry() {
				// create a new list entry as lonely
				// both next and prev pointing to this
				this->next = this;
				this->prev = this;
			}
			virtual ~ListEntry() {}
		
			/**
			 *  checks if the ListEntry is the list head
			 *  @return true if ListEntry is list head otherwise false
			 */
			virtual bool isListHead() const = 0;
		
			/**
			 *  a lonely ListEntry has no connection to other elements
			 *  @return true if ListEntry is lonely otherwise false
			 */
			bool isLonely() const { return (this->next == this && this->prev == this); }
		
		protected:
			
			/**
			 *  insert a given ListEntry after this one
			 *  @param entry the ListEntry to insert after this one
			 */
			inline void insertAsSuccessor(ListEntryContent<T> *entry) {
				assert (entry->isLonely());
			
				// mount the new element between this and this->next
				entry->next = this->next;
				entry->prev = this;
			
				// point this->next->prev to entry and _afterwards_ overwrite this->next
				this->next->prev = entry;
				this->next = entry;
			
				assert (not entry->isLonely());
			}
			
			/**
			 *  insert a given ListEntry in front of this one
			 *  @param entry the ListEntry to insert in front of this one
			 */
			inline void insertAsPredecessor(ListEntryContent<T> *entry) {
				assert (entry->isLonely());
				assert (not entry->isListHead());
			
				// mount the new element between this and this->prev
				entry->next = this;
				entry->prev = this->prev;
			
				// point this->prev->next to entry and _afterwards_ overwrite this->prev
				this->prev->next = entry;
				this->prev = entry;
			
				assert (not entry->isLonely());
			}
		
			/**
			 *  remove this element from it's list
			 *  the function connects this->next with this->prev leaving the complete list
			 *  in a consistent state. The ListEntry itself is lonely afterwards, but not
			 *  deleted!!
			 */
			virtual void removeFromList() = 0;
		};

		/**
		 *  Specialized ListEntry to contain a data entry of type T
		 */
		template<class T>
		class ListEntryContent
					: public ListEntry<T> {
		private:
			T mContent; // <-- the data content of this ListEntry

		public:
			ListEntryContent(Key content) {
				mContent = content;
			};

			/**
			 *  see ListEntry base class
			 */
			inline bool isListHead() const { return false; }

			/**
			 *  retrieve to content of this ListEntry
			 *  @return the content of this ListEntry
			 */
			inline T getContent() const { return mContent; }

			/**
			 *  see ListEntry base class
			 */
			inline void removeFromList() {
				assert (not this->isLonely());

				// remove this from list
				this->prev->next = this->next;
				this->next->prev = this->prev;

				// make this lonely
				this->next = this;
				this->prev = this;

				assert (this->isLonely());
			}
		};
	
		/**
		 *  Specialized ListEntry to form a list head.
		 *  Every list has exactly one list head which is also the entry point
		 *  in the list. It is used to manipulate the list.
		 */
		template<class T>
		class ListEntryHead
					: public ListEntry<T> {
		public:
			virtual ~ListEntryHead() {
				this->clear();
			}
		
			/**
			 *  remove all entries from the list
			 *  ListEntry objects are deleted but contained data keeps available
			 */
			void clear() {
				// delete all list entries
				ListEntry<T> *entry = this->next;
				ListEntry<T> *entryToDelete;
				while (not entry->isListHead()) {
					entryToDelete = entry;
					entry = entry->next;
					delete entryToDelete;
				}
			
				// reset the list to lonely
				this->next = this;
				this->prev = this;
			}
		
			/**
			 *  see ListEntry base class
			 */
			inline bool isListHead() const { return true; }
		
			/**
			 *  check if the list is empty
			 *  @return true if the list only contains the ListEntryHead otherwise false
			 */
			inline bool isEmpty() const { return this->isLonely(); }
		
			/**
			 *  retrieve the data in the front of the list
			 *  @return the first data object in the list
			 */
			inline T getFront() const {
				assert (not this->isEmpty());
				return this->next->getContent();
			}
			
			/**
			 *  retrieve the data in the back of the list
			 *  @return the last data object in the list
			 */
			inline T getBack() const {
				assert (not this->isEmpty());
				return this->prev->getContent();
			}
		
			/**
			 *  push a new data object to the end of the list
			 *  @param the data object to insert
			 *  @return the ListEntryContent structure which was wrapped around the data object
			 */
			inline ListEntryContent<T>* push_back(T content) {
				ListEntryContent<T> *newEntry = new ListEntryContent<T>(content);
				this->insertAsPredecessor(newEntry);
				return newEntry;
			}
		
			/**
			 *  push a new data object to the front of the list
			 *  @param the data object to insert
			 *  @return the ListEntryContent structure which was wrapped around the data object
			 */
			inline ListEntryContent<T>* push_front(T content) {
				ListEntryContent<T> *newEntry = new ListEntryContent<T>(content);
				this->insertAsSuccessor(newEntry);
				return newEntry;
			}
		
			/**
			 *  pop the first object of the list
			 *  the object is returned and removed from the list
			 *  @return the data object which resided in the first list entry
			 */
			inline T pop_front() {
				assert (not this->isEmpty());
				return pop(this->next);
			}

			/**
			 *  pop the last object of the list
			 *  the object is returned and removed from the list
			 *  @return the data object which resided in the last list entry
			 */
			inline T pop_back() {
				assert (not this->isEmpty());
				return pop(this->prev);
			}
		
			/**
			 *  take a list entry out of it's list and reinsert just behind this ListEntryHead
			 *  @param the ListEntry to be moved to the beginning of this list
			 */
			inline void moveToFront(ListEntryContent<T> *entry) {
				assert (not entry->isLonely());
			
				entry->removeFromList();
				this->insertAsSuccessor(entry);
			}

			/**
			 *  take a list entry out of it's list and reinsert at the end of this list
			 *  @param the ListEntry to be moved to the end of this list
			 */
			inline void moveToBack(ListEntryContent<T> *entry) {
				assert (not entry->isLonely());
			
				entry->removeFromList();
				this->insertAsPredecessor(entry);
			}
			
			/**
			 *  see ListEntry base class
			 */
			inline void removeFromList() { assert (false); }
		
		private:
			/**
			 *  pop a ListEntry from the list (arbitrary position)
			 *  the given ListEntry is removed from the list, deleted and it's data content is returned
			 *  @param poppedEntry the entry to be popped
			 *  @return the data object of the popped ListEntry
			 */
			inline T pop(ListEntry<T> *poppedEntry) {
				assert (not poppedEntry->isListHead());
			
				ListEntryContent<T> *popped = (ListEntryContent<T> *) poppedEntry;
				popped->removeFromList();
				T res = popped->getContent();
				delete popped;
				return res;
			}
		};
	
	public:
		/**
		 *  create a new LRU cache object
		 *  @param maxCacheSize the maximal size of the cache
		 */
		LruCache(const unsigned int maxCacheSize) {
			assert (maxCacheSize > 0);
		
			mCurrentCacheSize = 0;
			mMaxCacheSize = maxCacheSize;
			mLruList = new ListEntryHead<Key>();
		}
	
		virtual ~LruCache() {
			delete mLruList;
		}
	
		/**
		 *  insert a new key-value pair to the list
		 *  if the cache is already full, the least recently used object is removed
		 *  afterwards the new object is inserted
		 *  if the object is already present it is updated and moved back to the end
		 *  of the list
		 *  @param key the key where the value is saved
		 *  @param value the value of the cache entry
		 *  @return true on successful insertion otherwise false
		 */
		virtual bool insert(const Key key, const Value value) {
			// check if we have to update an existent entry
			CacheEntry entry;
			if (this->lookupCache(key, entry)) {
				entry.value = value;
				this->updateExistingEntry(key, entry);
				this->touchEntry(entry);
				return true;
			}
			
			// check if we have to make some space in the cache
			if (this->isFull()) {
				this->deleteOldestEntry();
			}

			// insert a new entry
			this->insertNewEntry(key, value);
			return true;
		}
	
		/**
		 *  retrieve an element from the cache
		 *  if the element was found, it will be marked as 'recently used' and returned
		 *  @param key the key to perform a lookup on
		 *  @param value (out) here the result is saved (in case of cache miss this is not altered)
		 *  @return true on successful lookup, false if key was not found
		 */
		virtual bool lookup(const Key key, Value &value) {
			CacheEntry entry;
			if (this->lookupCache(key, entry)) {
				// cache hit
				this->touchEntry(entry);
				value = entry.value;
				return true;
			}
			
			// cache miss
			return false;
		}
	
		/**
		 *  checks if the cache is filled completely
		 *  if yes, an insert of a new element will delete the least recently used one
		 *  @return true if cache is fully used otherwise false
		 */
		inline bool isFull() const { return mCurrentCacheSize >= mMaxCacheSize; }
		
		/**
		 *  checks if there is at least one element in the cache
		 *  @return true if cache is completely empty otherwise false
		 */
		inline bool isEmpty() const { return mCurrentCacheSize == 0; }
		
		/**
		 *  returns the current amount of entries in the cache
		 *  @return the number of entries currently in the cache
		 */
		inline unsigned int getNumberOfEntries() const { return mCurrentCacheSize; }
	
		/**
		 *  clears all elements from the cache
		 *  all memory of internal data structures will be freed but data of cache entries
		 *  may stay in use, we do not call delete on any user data
		 */
		void drop() {
			mCurrentCacheSize = 0;
			mLruList->clear();
			mCache.clear();
		}
	
		/**
		 *  change the size of the cache at runtime
		 *  if the new cache size is smaller than the current amount of entries
		 *  the least recently used ones are deleted until the cache is on the desired size
		 *  @param newSize the desired size of the cache
		 */
		void resize(const unsigned int newSize) {
			assert (newSize > 0);
		
			while (mCurrentCacheSize > newSize) {
				this->deleteOldestEntry();
			}
		
			mMaxCacheSize = newSize;
		}
	
	private:
		/**
		 *  this just performs a lookup in the cache
		 *  WITHOUT changing the LRU order
		 *  @param key the key to perform a lookup on
		 *  @param entry a pointer to the entry structure
		 *  @return true on successful lookup, false otherwise
		 */
		inline bool lookupCache(const Key key, CacheEntry &entry) {
			typename Cache::iterator foundElement = mCache.find(key);

			if (foundElement == mCache.end()) {
				// cache miss
				return false;
			}
			
			// cache hit
			entry = foundElement->second;
			return true;
		}
		
		/**
		 *  insert a new entry in the cache
		 *  wraps the user data in an internal data structure
		 *  @param key the key to save the value in
		 *  @param value the user data
		 */
		inline void insertNewEntry(const Key key, const Value value) {
			assert (not this->isFull());
		
			CacheEntry entry;
			entry.listEntry = mLruList->push_back(key);
			entry.value = value;
		
			mCache[key] = entry;
			mCurrentCacheSize++;
		}
		
		/**
		 *  update an entry which is already in the cache
		 *  this will not change the LRU order. Just the new data object is
		 *  associated with the given key value in the cache
		 *  @param key the key to save the value in
		 *  @param entry the CacheEntry structure to save in the cache
		 */
		inline void updateExistingEntry(const Key key, const CacheEntry entry) {
			mCache[key] = entry;
		}
	
		/**
		 *  touch an entry
		 *  the entry will be moved to the back of the LRU list to mark it
		 *  as 'recently used'... this saves the entry from being deleted
		 *  @param entry the CacheEntry to be touched (CacheEntry is the internal wrapper data structure)
		 */
		inline void touchEntry(const CacheEntry &entry) {
			mLruList->moveToBack(entry.listEntry);
		}
	
		/**
		 *  deletes the least recently used entry from the cache
		 */
		inline void deleteOldestEntry() {
			assert (not this->isEmpty());
		
			Key keyToDelete = mLruList->pop_front();
			mCache.erase(keyToDelete);
			
			mCurrentCacheSize--;
		}
	};
} // namespace cvmfs

#endif