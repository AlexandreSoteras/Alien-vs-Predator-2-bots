// compat/stlport_hashtable.h - a hash table that iterates in exactly the order
// STLport 4's SGI hashtable does.
//
// Why this exists: the AvP2 game code was built against STLport (the release's
// LT2/lithshared/stl) and enumerates hash_maps to number things. CButeMgr keeps
// the tags of an attribute file in a hash_map, and CCharacterButeMgr,
// CWeaponMgr, CSoundButeMgr, ... assign their indices in GetTags() order, i.e.
// in hash-table iteration order. Those indices (character set, weapon, ammo,
// ...) are sent to clients. A server built on std::unordered_map would number
// them differently from the retail client's cshell.dll and the two would
// disagree about which character or weapon an id means. So the order has to be
// STLport's, bucket for bucket.
//
// What is reproduced (STLport stl/_hashtable.h, stl/_hashtable.c):
//   - bucket counts come from the 28-entry prime list; a default-constructed
//     container asks for 100 and gets 193
//   - bucket = hash(key) % bucket_count
//   - a new element goes to the FRONT of its bucket's chain
//   - before every insert the table is resized for size()+1 elements: when that
//     exceeds the bucket count it grows to the next prime, old buckets are
//     walked in order and each node is pushed on the front of its new chain
//   - iteration walks the buckets in index order, each chain front to back
//   - the default hash functions (identity for integers, h = 5*h + c for
//     C strings and std::string)
//
// Written for this project; it contains no STLport code.
#pragma once
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace stlport_compat {

// ---- STLport's default hash functions (stl/_hash_fun.h, _string_hash.h) -----
inline size_t hash_c_string(const char* s)
{
    unsigned long h = 0;
    for (; *s; ++s)
        h = 5 * h + *s;          // plain (signed) char, as in STLport
    return size_t(h);
}

template <class Key> struct hash {};   // no generic definition, as in STLport
template <> struct hash<char*>              { size_t operator()(const char* s) const { return hash_c_string(s); } };
template <> struct hash<const char*>        { size_t operator()(const char* s) const { return hash_c_string(s); } };
template <> struct hash<char>               { size_t operator()(char x) const { return x; } };
template <> struct hash<unsigned char>      { size_t operator()(unsigned char x) const { return x; } };
template <> struct hash<signed char>        { size_t operator()(unsigned char x) const { return x; } };
template <> struct hash<short>              { size_t operator()(short x) const { return x; } };
template <> struct hash<unsigned short>     { size_t operator()(unsigned short x) const { return x; } };
template <> struct hash<int>                { size_t operator()(int x) const { return x; } };
template <> struct hash<unsigned int>       { size_t operator()(unsigned int x) const { return x; } };
template <> struct hash<long>               { size_t operator()(long x) const { return x; } };
template <> struct hash<unsigned long>      { size_t operator()(unsigned long x) const { return x; } };
template <> struct hash<std::string>
{
    size_t operator()(const std::string& s) const
    {
        unsigned long h = 0;
        const char* data = s.data();
        for (size_t i = 0, len = s.size(); i < len; ++i)
            h = 5 * h + data[i];
        return size_t(h);
    }
};

template <class T> struct identity   { const T& operator()(const T& x) const { return x; } };
template <class P> struct select1st  { const typename P::first_type& operator()(const P& x) const { return x.first; } };

inline size_t next_prime(size_t n)
{
    static const unsigned long primes[28] = {
        53ul,         97ul,         193ul,       389ul,       769ul,
        1543ul,       3079ul,       6151ul,      12289ul,     24593ul,
        49157ul,      98317ul,      196613ul,    393241ul,    786433ul,
        1572869ul,    3145739ul,    6291469ul,   12582917ul,  25165843ul,
        50331653ul,   100663319ul,  201326611ul, 402653189ul, 805306457ul,
        1610612741ul, 3221225473ul, 4294967291ul
    };
    for (int i = 0; i < 28; ++i)          // lower_bound
        if (primes[i] >= n)
            return primes[i];
    return primes[27];
}

// ---- the table ----------------------------------------------------------------
template <class Val, class Key, class HashFn, class ExtractKey, class EqualKey>
class hashtable
{
    struct Node { Node* next; Val val; Node(const Val& v) : next(0), val(v) {} };

public:
    typedef Key    key_type;
    typedef Val    value_type;
    typedef size_t size_type;

    template <class Ref, class Ptr, class Table>
    class iter
    {
    public:
        typedef std::forward_iterator_tag iterator_category;
        typedef Val       value_type;
        typedef ptrdiff_t difference_type;
        typedef Ptr       pointer;
        typedef Ref       reference;

        iter() : cur(0), ht(0) {}
        iter(Node* n, Table* t) : cur(n), ht(t) {}
        template <class R2, class P2, class T2>
        iter(const iter<R2, P2, T2>& o) : cur(o.cur), ht(o.ht) {}

        Ref operator*() const  { return cur->val; }
        Ptr operator->() const { return &cur->val; }
        iter& operator++()
        {
            Node* old = cur;
            cur = cur->next;
            if (!cur) {                   // _M_skip_to_next
                size_t bucket = ht->bkt_num(old->val);
                size_t count = ht->buckets.size();
                while (!cur && ++bucket < count)
                    cur = ht->buckets[bucket];
            }
            return *this;
        }
        iter operator++(int) { iter tmp = *this; ++*this; return tmp; }
        template <class R2, class P2, class T2> bool operator==(const iter<R2, P2, T2>& o) const { return cur == o.cur; }
        template <class R2, class P2, class T2> bool operator!=(const iter<R2, P2, T2>& o) const { return cur != o.cur; }

        Node*  cur;
        Table* ht;
    };
    typedef iter<Val&, Val*, hashtable>                   iterator;
    typedef iter<const Val&, const Val*, const hashtable> const_iterator;

    explicit hashtable(size_type n) : num_elements(0) { buckets.assign(next_prime(n), (Node*)0); }
    hashtable(const hashtable& o) : num_elements(0), hasher(o.hasher), equals(o.equals) { copy_from(o); }
    hashtable& operator=(const hashtable& o)
    {
        if (&o != this) { clear(); hasher = o.hasher; equals = o.equals; copy_from(o); }
        return *this;
    }
    ~hashtable() { clear(); }

    size_type size() const         { return num_elements; }
    bool      empty() const        { return num_elements == 0; }
    size_type bucket_count() const { return buckets.size(); }

    void swap(hashtable& o)
    {
        std::swap(hasher, o.hasher);
        std::swap(equals, o.equals);
        buckets.swap(o.buckets);
        std::swap(num_elements, o.num_elements);
    }

    iterator begin()
    {
        for (size_type n = 0; n < buckets.size(); ++n)
            if (buckets[n]) return iterator(buckets[n], this);
        return end();
    }
    const_iterator begin() const
    {
        for (size_type n = 0; n < buckets.size(); ++n)
            if (buckets[n]) return const_iterator(buckets[n], this);
        return end();
    }
    iterator       end()       { return iterator(0, this); }
    const_iterator end() const { return const_iterator(0, this); }

    std::pair<iterator, bool> insert_unique(const Val& obj)
    {
        resize(num_elements + 1);
        return insert_unique_noresize(obj);
    }

    std::pair<iterator, bool> insert_unique_noresize(const Val& obj)
    {
        const size_type n = bkt_num(obj);
        Node* first = buckets[n];
        for (Node* cur = first; cur; cur = cur->next)
            if (equals(get_key(cur->val), get_key(obj)))
                return std::pair<iterator, bool>(iterator(cur, this), false);
        Node* tmp = new Node(obj);
        tmp->next = first;
        buckets[n] = tmp;
        ++num_elements;
        return std::pair<iterator, bool>(iterator(tmp, this), true);
    }

    Val& find_or_insert(const Val& obj)
    {
        resize(num_elements + 1);
        const size_type n = bkt_num(obj);
        Node* first = buckets[n];
        for (Node* cur = first; cur; cur = cur->next)
            if (equals(get_key(cur->val), get_key(obj)))
                return cur->val;
        Node* tmp = new Node(obj);
        tmp->next = first;
        buckets[n] = tmp;
        ++num_elements;
        return tmp->val;
    }

    iterator find(const Key& key)
    {
        Node* cur = buckets[bkt_num_key(key)];
        while (cur && !equals(get_key(cur->val), key)) cur = cur->next;
        return iterator(cur, this);
    }
    const_iterator find(const Key& key) const
    {
        Node* cur = buckets[bkt_num_key(key)];
        while (cur && !equals(get_key(cur->val), key)) cur = cur->next;
        return const_iterator(cur, this);
    }
    size_type count(const Key& key) const
    {
        size_type result = 0;
        for (const Node* cur = buckets[bkt_num_key(key)]; cur; cur = cur->next)
            if (equals(get_key(cur->val), key)) ++result;
        return result;
    }

    size_type erase(const Key& key)
    {
        const size_type n = bkt_num_key(key);
        Node* first = buckets[n];
        size_type erased = 0;
        if (first) {
            Node* cur = first;
            Node* next = cur->next;
            while (next) {
                if (equals(get_key(next->val), key)) {
                    cur->next = next->next;
                    delete next;
                    next = cur->next;
                    ++erased;
                    --num_elements;
                } else {
                    cur = next;
                    next = cur->next;
                }
            }
            if (equals(get_key(first->val), key)) {
                buckets[n] = first->next;
                delete first;
                ++erased;
                --num_elements;
            }
        }
        return erased;
    }

    void erase(const const_iterator& it)
    {
        Node* p = it.cur;
        if (!p) return;
        const size_type n = bkt_num(p->val);
        Node* cur = buckets[n];
        if (cur == p) {
            buckets[n] = cur->next;
            delete cur;
            --num_elements;
        } else {
            Node* next = cur->next;
            while (next) {
                if (next == p) {
                    cur->next = next->next;
                    delete next;
                    --num_elements;
                    break;
                }
                cur = next;
                next = cur->next;
            }
        }
    }

    void resize(size_type num_elements_hint)
    {
        const size_type old_n = buckets.size();
        if (num_elements_hint > old_n) {
            const size_type n = next_prime(num_elements_hint);
            if (n > old_n) {
                std::vector<Node*> tmp(n, (Node*)0);
                for (size_type bucket = 0; bucket < old_n; ++bucket) {
                    Node* first = buckets[bucket];
                    while (first) {
                        size_type new_bucket = bkt_num(first->val, n);
                        buckets[bucket] = first->next;
                        first->next = tmp[new_bucket];
                        tmp[new_bucket] = first;
                        first = buckets[bucket];
                    }
                }
                buckets.swap(tmp);
            }
        }
    }

    void clear()
    {
        for (size_type i = 0; i < buckets.size(); ++i) {
            Node* cur = buckets[i];
            while (cur) {
                Node* next = cur->next;
                delete cur;
                cur = next;
            }
            buckets[i] = 0;
        }
        num_elements = 0;
    }

private:
    size_type bkt_num_key(const Key& key) const              { return hasher(key) % buckets.size(); }
    size_type bkt_num_key(const Key& key, size_type n) const { return hasher(key) % n; }
    size_type bkt_num(const Val& obj) const                  { return bkt_num_key(get_key(obj)); }
    size_type bkt_num(const Val& obj, size_type n) const     { return bkt_num_key(get_key(obj), n); }

    void copy_from(const hashtable& o)       // same bucket count, same chain order
    {
        buckets.assign(o.buckets.size(), (Node*)0);
        for (size_type i = 0; i < o.buckets.size(); ++i) {
            const Node* cur = o.buckets[i];
            if (cur) {
                Node* copy = new Node(cur->val);
                buckets[i] = copy;
                for (const Node* next = cur->next; next; next = next->next) {
                    copy->next = new Node(next->val);
                    copy = copy->next;
                }
            }
        }
        num_elements = o.num_elements;
    }

    std::vector<Node*> buckets;
    size_type          num_elements;
    HashFn             hasher;
    EqualKey           equals;
    ExtractKey         get_key;
};

} // namespace stlport_compat
