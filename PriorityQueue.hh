#pragma once

#include <vector>
#include "TaggedLow.hh"
#include "Transaction.hh"
#include "versioned_value.hh"
#include "VersionFunctions.hh"

enum Status{
    AVAILABLE,
    EMPTY,
    BUSY,
};

template <typename T>
struct HeapNode{
    typedef versioned_value_struct<T> versioned_value;
    typedef TransactionTid::type Version;
public:
    versioned_value* val;
    Version ver;
    Status status;
    int owner;
    HeapNode(versioned_value* val_) : val(val_), ver(0), status(BUSY) {
        owner = Transaction::threadid;
    }
    
    bool amOwner() {
        return status == BUSY && owner == Transaction::threadid;
    }
};

template <typename T, bool Opacity = false>
class PriorityQueue: public Shared {
    typedef TransactionTid::type Version;
    typedef VersionFunctions<Version> Versioning;
    typedef versioned_value_struct<T> versioned_value;
    
    static constexpr TransItem::flags_type insert_tag = TransItem::user0_bit;
    static constexpr TransItem::flags_type delete_tag = TransItem::user0_bit<<1;
    static constexpr TransItem::flags_type dirty_tag = TransItem::user0_bit<<2;
    
    static constexpr Version insert_bit = TransactionTid::user_bit1;
    static constexpr Version delete_bit = TransactionTid::user_bit1<<1;
    static constexpr Version dirty_bit = TransactionTid::user_bit1<<2;
    
    static constexpr int NO_ONE = -1;
    
    static constexpr int pop_key = -2;
public:
    PriorityQueue() : heap_() {
        size_ = 0;
        heaplock_ = 0;
        poplock_ = 0;
        popversion_ = 0;
    }
    
    // Concurrently adds v to the priority queue
    void add(versioned_value* v) {
        lock(&heaplock_);
        int child = size_++;
        HeapNode<T>* new_node = new HeapNode<T>(v);
        if (child >= heap_.size()) {
            heap_.push_back(new_node);
        } else
            heap_[child] = new_node;
        unlock(&heaplock_);
        while (child > 0) {
            int parent = (child - 1) / 2;
            versioned_value* before = heap_[parent]->val;
            lock(&(heap_[parent]->ver));
            lock(&(heap_[child]->ver));
            int old = child;
            if (heap_[parent]->status == AVAILABLE && heap_[child]->amOwner()) {
                versioned_value* parent_val = heap_[parent]->val;
                if (heap_[child]->val->read_value() > parent_val->read_value()) {
                    swap(child, parent);
                    child = parent;
                    if (is_dirty(parent_val->version())) {
                        auto item = Sto::item(this, parent_val);
                        if (!has_dirty(item)) {
                            // Some other concurrent transaction did a pop and dirtied the parent - so abort
                            heap_[child]->status = AVAILABLE;
                            heap_[child]->owner = NO_ONE;
                            unlock(&(heap_[old]->ver));
                            unlock(&(heap_[parent]->ver));
                            Sto::abort();
                            return;
                        }
                    }
                } else {
                    heap_[child]->status = AVAILABLE;
                    heap_[child]->owner = NO_ONE;
                    unlock(&(heap_[old]->ver));
                    unlock(&(heap_[parent]->ver));
                    return;
                }
            } else if (!heap_[child]->amOwner()) {
                child = parent;
            }
            
            unlock(&(heap_[old]->ver));
            unlock(&(heap_[parent]->ver));
        }
        
        if (child == 0) {
            lock(&(heap_[0]->ver));
            if (heap_[0]->amOwner()) {
                heap_[0]->status = AVAILABLE;
                heap_[0]->owner = NO_ONE;
            }
            unlock(&(heap_[0]->ver));
        }
    }
    
    // Concurrently removes the maximum element from the heap
    versioned_value* removeMax(versioned_value* expVal = NULL) {
        lock(&heaplock_);
        assert(is_locked(heaplock_));
        int bottom  = --size_;
        if (bottom == 0) {
            versioned_value* res = heap_[0]->val;
            unlock(&heaplock_);
            return res;
        }
        lock(&heap_[bottom]->ver);
        lock(&heap_[0]->ver);
        versioned_value* res = heap_[0]->val;
        unlock(&heaplock_);
        if (expVal != NULL && res != expVal) {
            return NULL;
        }
        heap_[0]->status = EMPTY;
        heap_[0]->owner = NO_ONE;
        swap(bottom, 0);
        assert(heap_[bottom]->status == EMPTY);
        unlock(&heap_[bottom]->ver);
        
        int child = 0;
        int parent = 0;
        while (2*parent < size_ - 2) {
            int left = parent * 2 + 1;
            int right = (parent * 2) + 2;
            assert(right <= size_);
            lock(&heap_[left]->ver);
            lock(&heap_[right]->ver);
            if (heap_[left]->status == EMPTY) {
                unlock(&heap_[right]->ver);
                unlock(&heap_[left]->ver);
                break;
            } else if (heap_[right]->status == EMPTY || heap_[left]->val->read_value() > heap_[right]->val->read_value()) {
                unlock(&heap_[right]->ver);
                child = left;
            } else {
                unlock(&heap_[left]->ver);
                child = right;
            }
            if (heap_[child]->val->read_value() > heap_[parent]->val->read_value()) {
                swap(parent, child);
                unlock(&heap_[parent]->ver);
                parent = child;
            } else {
                unlock(&heap_[child]->ver);
                break;
            }
        }
        unlock(&heap_[parent]->ver);
        return res;
    }
    
    // TODO: should make this method concurrent with the other methods.
    versioned_value* getMax() {
        assert(is_locked(poplock_));
        if (size_ == 0) {
            return NULL;
        }
        while(1) {
            versioned_value* val = heap_[0]->val;
            auto item = Sto::item(this, val);
            if (is_inserted(val->version())) {
                if (has_insert(item)) {
                    // push then pop
                    return val;
                } else {
                    // Some other transaction is inserting a node with high priority
                    unlock(&poplock_);
                    Sto::abort();
                    return NULL;
                }
            } else if (is_deleted(val->version())) { // TODO: this is not thread safe
                removeMax(val);
            } else {
                return val;
            }
        }
    }
    
    void push(T v) {
        versioned_value* val = versioned_value::make(v, TransactionTid::increment_value + insert_bit);
        add(val);
        Sto::item(this, val).add_write(v).add_flags(insert_tag);
    }
    
    void pop() {
        if (size_ == 0) {
            return;
        }
        lock(&poplock_);
        // TODO: deal with deleted heads
        versioned_value* val = removeMax();
        // TODO: what happens if there is a push with high priority right at this point
        versioned_value* new_head = getMax();
        if (new_head != NULL) {
            mark_dirty(&new_head->version());
        }
        // TODO: should deal properly with the case when new_head is null.
        unlock(&poplock_);
        if (new_head != NULL) {
            Sto::item(this, new_head).add_write(0).add_flags(dirty_tag);// Adding new_head as key so that we can track that this transaction dirtied it.
        }
        auto item = Sto::item(this, val).add_read(val->version());
        item.add_write(new_head).add_flags(delete_tag);
        
        Sto::item(this, pop_key).add_write(0);
    }
    
    T top() {
        Sto::item(this, pop_key).add_read(popversion_);
        acquire_fence();
        lock(&poplock_);
        versioned_value* val = getMax();
        unlock(&poplock_);
        T retval = val->read_value();
        Sto::item(this, val).add_read(val->version());
        return retval;
    }
    
    int size() {
        return heap_.size(); // TODO: this is not transactional yet
    }
    
    void lock(versioned_value *e) {
        lock(&e->version());
    }
    void unlock(versioned_value *e) {
        unlock(&e->version());
    }
    
    void lock(TransItem& item) {
        if (item.key<int>() == pop_key){
            lock(&popversion_);
        } else {
            lock(item.key<versioned_value*>());
        }
    }
    
    void unlock(TransItem& item) {
        if (item.key<int>() == pop_key){
            unlock(&popversion_);
        } else {
            unlock(item.key<versioned_value*>());
        }
    }
    
    bool check(const TransItem& item, const Transaction& trans){
        if (item.key<int>() == pop_key) {
            auto lv = popversion_;
            return TransactionTid::same_version(lv, item.template read_value<Version>())
            && (!is_locked(lv) || item.has_lock(trans));
        } else {
            auto e = item.key<versioned_value*>();
            auto read_version = item.template read_value<Version>();
            bool same_version = (read_version ^ e->version()) <= (dirty_bit + TransactionTid::lock_bit);
            bool not_locked = (!is_locked(e->version()) || item.has_lock(trans));
            bool not_dirty = (!is_dirty(read_version) || !is_dirty(e->version()) || item.has_lock(trans)); // here, if the item is locked by trans, it means that the current transaction itself dirited this item.
            return same_version && not_locked && not_dirty;
        }
    }
    
    
    void install(TransItem& item, const Transaction& t) {
        if (item.key<int>() == pop_key){
            if (Opacity) {
                TransactionTid::set_version(popversion_, t.commit_tid());
            } else {
                TransactionTid::inc_invalid_version(popversion_);
            }
        } else {
            auto e = item.key<versioned_value*>();
            assert(is_locked(e->version()));
            if (has_insert(item)) {
                erase_inserted(&e->version());
            }
            if (has_delete(item)) {
                auto new_head = item.template write_value<versioned_value*>();
                if (new_head != NULL) {
                    if (is_dirty(new_head->version()))
                      erase_dirty(&new_head->version());
                }
            }
        }
    }
    
    void cleanup(TransItem& item, bool committed) {
        if (!committed) {
            if (has_insert(item)) {
                auto e = item.key<versioned_value*>();
                mark_deleted(&e->version()); // TODO: order of these maynot be right?
                erase_inserted(&e->version());
            } else if (has_delete(item)) {
                auto e = item.key<versioned_value*>();
                auto v = e->read_value();
                versioned_value* val = versioned_value::make(v, TransactionTid::increment_value);
                add(val);
                auto new_head = item.template write_value<versioned_value*>();
                if (new_head) {
                    lock(&new_head->version());
                    TransactionTid::inc_invalid_version(new_head->version());
                    assert(is_dirty(new_head->version()));
                    erase_dirty(&new_head->version());
                    unlock(&new_head->version());
                }
            }
        }
    }
    
    // Used for debugging
    void print() {
        for (int i =0; i < size_; i++) {
            std::cout << heap_[i].val->read_value() << "[" << (!is_inserted(heap_[i].val->version()) && !is_deleted(heap_[i].val->version())) << "] ";
        }
        std::cout << std::endl;
    }
    
    
private:
    static void lock(Version *v) {
        TransactionTid::lock(*v);
    }
    
    static void unlock(Version *v) {
        TransactionTid::unlock(*v);
    }
    
    static bool is_locked(Version v) {
        return TransactionTid::is_locked(v);
    }

    static bool has_insert(const TransItem& item) {
        return item.flags() & insert_tag;
    }
    static bool has_delete(const TransItem& item) {
        return item.flags() & delete_tag;
    }
    
    static bool has_dirty(const TransItem& item) {
        return item.flags() & dirty_tag;
    }
    
    static bool is_inserted(Version v) {
        return v & insert_bit;
    }
    
    static void erase_inserted(Version* v) {
        *v = *v & (~insert_bit);
    }
    
    static void mark_inserted(Version* v) {
        *v = *v | insert_bit;
    }
    
    static bool is_dirty(Version v) {
        return v & dirty_bit;
    }
    
    static void erase_dirty(Version* v) {
        *v = *v & (~dirty_bit);
    }
    
    static void mark_dirty(Version* v) {
        *v = *v | dirty_bit;
    }
            
    static bool is_deleted(Version v) {
        return v & delete_bit;
    }
            
    static void erase_deleted(Version* v) {
        *v = *v & (~delete_bit);
    }
            
    static void mark_deleted(Version* v) {
        *v = *v | delete_bit;
    }


    void swap(int i, int j) {
        HeapNode<T> tmp = *(heap_[i]);
        heap_[i]->val = heap_[j]->val;
        heap_[i]->status = heap_[j]->status;
        heap_[i]->owner = heap_[j]->owner;
        heap_[j]->val = tmp.val;
        heap_[j]->status = tmp.status;
        heap_[j]->owner = tmp.owner;
    }
    std::vector<HeapNode<T> *> heap_;
    Version heaplock_;
    Version poplock_;
    Version popversion_;
    int size_;
    
};
