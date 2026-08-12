#include <bits/stdc++.h>
using namespace std;

static const uint32_t BLOCK_SIZE = 4096;
static const uint32_t MAX_KEY_LEN = 64;
static const uint32_t MAX_KEYS = 55;
static const uint32_t MIN_KEYS_LEAF = (MAX_KEYS + 1) / 2;        // 28
static const uint32_t MIN_KEYS_INTERNAL = (MAX_KEYS + 1) / 2 - 1; // 27

#pragma pack(push, 1)
struct Entry {
    char key[MAX_KEY_LEN];
    int value;
};

struct Node {
    uint8_t is_leaf;
    uint8_t pad[3];
    int32_t num_keys;
    uint32_t parent;
    uint32_t next; // leaf next sibling
    uint32_t prev; // leaf previous sibling
    Entry entries[MAX_KEYS + 1];
    uint32_t children[MAX_KEYS + 2];
};

struct Header {
    char magic[8];
    uint32_t root_block;
    uint32_t block_count;
    uint32_t free_list_head;
    uint32_t reserved[5];
};
#pragma pack(pop)

static_assert(sizeof(Entry) == 68, "Entry size mismatch");
static_assert(sizeof(Node) <= BLOCK_SIZE, "Node exceeds block size");
static_assert(sizeof(Header) == 40, "Header size mismatch");

class BPTree {
    string filename_;
    FILE* f_;
    Header header_;
    unordered_map<uint32_t, Node> cache_;
    unordered_set<uint32_t> dirty_;

    static int cmp_entry(const Entry& a, const Entry& b) {
        int c = memcmp(a.key, b.key, MAX_KEY_LEN);
        if (c != 0) return c;
        if (a.value < b.value) return -1;
        if (a.value > b.value) return 1;
        return 0;
    }

    static void set_entry(Entry& e, const string& k, int v) {
        size_t n = k.size();
        if (n > MAX_KEY_LEN) n = MAX_KEY_LEN;
        memcpy(e.key, k.data(), n);
        if (n < MAX_KEY_LEN) memset(e.key + n, 0, MAX_KEY_LEN - n);
        e.value = v;
    }

    static int upper_bound(const Node& node, const Entry& target) {
        int lo = 0, hi = node.num_keys;
        while (lo < hi) {
            int mid = (lo + hi) >> 1;
            if (cmp_entry(node.entries[mid], target) <= 0)
                lo = mid + 1;
            else
                hi = mid;
        }
        return lo;
    }
    static int lower_bound(const Node& node, const Entry& target) {
        int lo = 0, hi = node.num_keys;
        while (lo < hi) {
            int mid = (lo + hi) >> 1;
            if (cmp_entry(node.entries[mid], target) < 0)
                lo = mid + 1;
            else
                hi = mid;
        }
        return lo;
    }

    void read_header() {
        fseek(f_, 0, SEEK_SET);
        fread(&header_, sizeof(Header), 1, f_);
    }

    void write_header() {
        fseek(f_, 0, SEEK_SET);
        fwrite(&header_, sizeof(Header), 1, f_);
    }

    void read_node(uint32_t bid, Node& node) {
        auto it = cache_.find(bid);
        if (it != cache_.end()) {
            node = it->second;
            return;
        }
        fseek(f_, (long)bid * BLOCK_SIZE, SEEK_SET);
        size_t r = fread(&node, sizeof(Node), 1, f_);
        if (r != 1) memset(&node, 0, sizeof(Node));
        cache_[bid] = node;
    }

    void write_node(uint32_t bid, const Node& node) {
        cache_[bid] = node;
        dirty_.insert(bid);
    }

    uint32_t alloc_block() {
        if (header_.free_list_head != 0) {
            Node node;
            uint32_t bid = header_.free_list_head;
            read_node(bid, node);
            header_.free_list_head = node.next;
            write_header();
            return bid;
        }
        uint32_t bid = header_.block_count++;
        write_header();
        return bid;
    }

    void free_block(uint32_t bid) {
        Node node;
        memset(&node, 0, sizeof(Node));
        node.next = header_.free_list_head;
        fseek(f_, (long)bid * BLOCK_SIZE, SEEK_SET);
        fwrite(&node, sizeof(Node), 1, f_);
        header_.free_list_head = bid;
        write_header();
        cache_.erase(bid);
        dirty_.erase(bid);
    }

    uint32_t get_parent(uint32_t bid) {
        Node node;
        read_node(bid, node);
        return node.parent;
    }

    void set_parent(uint32_t bid, uint32_t parent) {
        Node node;
        read_node(bid, node);
        node.parent = parent;
        write_node(bid, node);
    }

    static int child_position(const Node& parent, uint32_t child) {
        for (int i = 0; i <= parent.num_keys; ++i)
            if (parent.children[i] == child) return i;
        return -1;
    }

    uint32_t find_leaf(const Entry& target) {
        if (header_.root_block == 0) return 0;
        uint32_t cur = header_.root_block;
        Node node;
        while (true) {
            read_node(cur, node);
            if (node.is_leaf) return cur;
            int pos = upper_bound(node, target);
            cur = node.children[pos];
        }
    }

    void insert_into_parent(uint32_t left_id, const Entry& sep, uint32_t right_id) {
        uint32_t parent_id = get_parent(left_id);
        if (parent_id == 0) {
            uint32_t new_root = alloc_block();
            Node root;
            memset(&root, 0, sizeof(Node));
            root.is_leaf = 0;
            root.num_keys = 1;
            root.entries[0] = sep;
            root.children[0] = left_id;
            root.children[1] = right_id;
            write_node(new_root, root);
            set_parent(left_id, new_root);
            set_parent(right_id, new_root);
            header_.root_block = new_root;
            write_header();
            return;
        }
        Node parent;
        read_node(parent_id, parent);
        int pos = child_position(parent, left_id);
        for (int i = parent.num_keys - 1; i >= pos; --i)
            parent.entries[i + 1] = parent.entries[i];
        for (int i = parent.num_keys; i >= pos + 1; --i)
            parent.children[i + 1] = parent.children[i];
        parent.entries[pos] = sep;
        parent.children[pos + 1] = right_id;
        parent.num_keys++;
        write_node(parent_id, parent);
        if (parent.num_keys > (int)MAX_KEYS)
            split_internal(parent_id);
    }

    void split_leaf(uint32_t leaf_id) {
        Node leaf;
        read_node(leaf_id, leaf);
        uint32_t new_id = alloc_block();
        Node new_node;
        memset(&new_node, 0, sizeof(Node));
        new_node.is_leaf = 1;
        int mid = leaf.num_keys / 2;
        new_node.num_keys = leaf.num_keys - mid;
        memcpy(new_node.entries, leaf.entries + mid, new_node.num_keys * sizeof(Entry));
        leaf.num_keys = mid;
        new_node.next = leaf.next;
        new_node.prev = leaf_id;
        leaf.next = new_id;
        new_node.parent = leaf.parent;
        if (new_node.next != 0) {
            Node next_node;
            read_node(new_node.next, next_node);
            next_node.prev = new_id;
            write_node(new_node.next, next_node);
        }
        write_node(new_id, new_node);
        write_node(leaf_id, leaf);
        insert_into_parent(leaf_id, new_node.entries[0], new_id);
    }

    void split_internal(uint32_t node_id) {
        Node node;
        read_node(node_id, node);
        uint32_t new_id = alloc_block();
        Node new_node;
        memset(&new_node, 0, sizeof(Node));
        new_node.is_leaf = 0;
        int mid = node.num_keys / 2;
        Entry sep = node.entries[mid];
        new_node.num_keys = node.num_keys - mid - 1;
        memcpy(new_node.entries, node.entries + mid + 1, new_node.num_keys * sizeof(Entry));
        memcpy(new_node.children, node.children + mid + 1, (new_node.num_keys + 1) * sizeof(uint32_t));
        node.num_keys = mid;
        new_node.parent = node.parent;
        write_node(new_id, new_node);
        write_node(node_id, node);
        for (int i = 0; i <= new_node.num_keys; ++i)
            set_parent(new_node.children[i], new_id);
        insert_into_parent(node_id, sep, new_id);
    }

    void handle_leaf_underflow(uint32_t leaf_id) {
        if (leaf_id == header_.root_block) return;
        Node leaf;
        read_node(leaf_id, leaf);
        if (leaf.num_keys >= (int)MIN_KEYS_LEAF) return;
        uint32_t parent_id = leaf.parent;
        Node parent;
        read_node(parent_id, parent);
        int pos = child_position(parent, leaf_id);
        // try borrow from left sibling
        if (pos > 0) {
            uint32_t left_id = parent.children[pos - 1];
            Node left;
            read_node(left_id, left);
            if (left.num_keys > (int)MIN_KEYS_LEAF) {
                // shift leaf entries right
                for (int i = leaf.num_keys - 1; i >= 0; --i)
                    leaf.entries[i + 1] = leaf.entries[i];
                leaf.entries[0] = left.entries[left.num_keys - 1];
                leaf.num_keys++;
                left.num_keys--;
                parent.entries[pos - 1] = leaf.entries[0];
                write_node(left_id, left);
                write_node(leaf_id, leaf);
                write_node(parent_id, parent);
                return;
            }
        }
        // try borrow from right sibling
        if (pos < parent.num_keys) {
            uint32_t right_id = parent.children[pos + 1];
            Node right;
            read_node(right_id, right);
            if (right.num_keys > (int)MIN_KEYS_LEAF) {
                leaf.entries[leaf.num_keys] = right.entries[0];
                leaf.num_keys++;
                for (int i = 1; i < right.num_keys; ++i)
                    right.entries[i - 1] = right.entries[i];
                right.num_keys--;
                parent.entries[pos] = right.entries[0];
                write_node(right_id, right);
                write_node(leaf_id, leaf);
                write_node(parent_id, parent);
                return;
            }
        }
        // merge
        if (pos < parent.num_keys) {
            uint32_t right_id = parent.children[pos + 1];
            Node right;
            read_node(right_id, right);
            memcpy(leaf.entries + leaf.num_keys, right.entries, right.num_keys * sizeof(Entry));
            leaf.num_keys += right.num_keys;
            leaf.next = right.next;
            if (right.next != 0) {
                Node next_node;
                read_node(right.next, next_node);
                next_node.prev = leaf_id;
                write_node(right.next, next_node);
            }
            write_node(leaf_id, leaf);
            // remove separator at pos and child at pos+1
            for (int i = pos + 1; i < parent.num_keys; ++i)
                parent.entries[i - 1] = parent.entries[i];
            for (int i = pos + 2; i <= parent.num_keys; ++i)
                parent.children[i - 1] = parent.children[i];
            parent.num_keys--;
            write_node(parent_id, parent);
            free_block(right_id);
            if (parent.num_keys < (int)MIN_KEYS_INTERNAL)
                handle_internal_underflow(parent_id);
        } else {
            uint32_t left_id = parent.children[pos - 1];
            Node left;
            read_node(left_id, left);
            memcpy(left.entries + left.num_keys, leaf.entries, leaf.num_keys * sizeof(Entry));
            left.num_keys += leaf.num_keys;
            left.next = leaf.next;
            if (leaf.next != 0) {
                Node next_node;
                read_node(leaf.next, next_node);
                next_node.prev = left_id;
                write_node(leaf.next, next_node);
            }
            write_node(left_id, left);
            // remove separator at pos-1 and child at pos
            for (int i = pos; i < parent.num_keys; ++i)
                parent.entries[i - 1] = parent.entries[i];
            for (int i = pos + 1; i <= parent.num_keys; ++i)
                parent.children[i - 1] = parent.children[i];
            parent.num_keys--;
            write_node(parent_id, parent);
            free_block(leaf_id);
            if (parent.num_keys < (int)MIN_KEYS_INTERNAL)
                handle_internal_underflow(parent_id);
        }
    }

    void handle_internal_underflow(uint32_t node_id) {
        if (node_id == header_.root_block) {
            Node root;
            read_node(node_id, root);
            if (root.num_keys == 0 && !root.is_leaf && root.children[0] != 0) {
                uint32_t new_root = root.children[0];
                set_parent(new_root, 0);
                free_block(node_id);
                header_.root_block = new_root;
                write_header();
            }
            return;
        }
        Node node;
        read_node(node_id, node);
        if (node.num_keys >= (int)MIN_KEYS_INTERNAL) return;
        uint32_t parent_id = node.parent;
        Node parent;
        read_node(parent_id, parent);
        int pos = child_position(parent, node_id);
        // borrow from left
        if (pos > 0) {
            uint32_t left_id = parent.children[pos - 1];
            Node left;
            read_node(left_id, left);
            if (left.num_keys > (int)MIN_KEYS_INTERNAL) {
                // shift node's entries and children right
                for (int i = node.num_keys - 1; i >= 0; --i)
                    node.entries[i + 1] = node.entries[i];
                for (int i = node.num_keys; i >= 0; --i)
                    node.children[i + 1] = node.children[i];
                node.entries[0] = parent.entries[pos - 1];
                node.children[0] = left.children[left.num_keys];
                node.num_keys++;
                parent.entries[pos - 1] = left.entries[left.num_keys - 1];
                left.num_keys--;
                set_parent(node.children[0], node_id);
                write_node(left_id, left);
                write_node(node_id, node);
                write_node(parent_id, parent);
                return;
            }
        }
        // borrow from right
        if (pos < parent.num_keys) {
            uint32_t right_id = parent.children[pos + 1];
            Node right;
            read_node(right_id, right);
            if (right.num_keys > (int)MIN_KEYS_INTERNAL) {
                node.entries[node.num_keys] = parent.entries[pos];
                node.children[node.num_keys + 1] = right.children[0];
                node.num_keys++;
                parent.entries[pos] = right.entries[0];
                for (int i = 1; i < right.num_keys; ++i)
                    right.entries[i - 1] = right.entries[i];
                for (int i = 1; i <= right.num_keys; ++i)
                    right.children[i - 1] = right.children[i];
                right.num_keys--;
                set_parent(node.children[node.num_keys], node_id);
                write_node(node_id, node);
                write_node(right_id, right);
                write_node(parent_id, parent);
                return;
            }
        }
        // merge
        if (pos < parent.num_keys) {
            uint32_t right_id = parent.children[pos + 1];
            Node right;
            read_node(right_id, right);
            node.entries[node.num_keys] = parent.entries[pos];
            memcpy(node.entries + node.num_keys + 1, right.entries, right.num_keys * sizeof(Entry));
            memcpy(node.children + node.num_keys + 1, right.children, (right.num_keys + 1) * sizeof(uint32_t));
            node.num_keys += 1 + right.num_keys;
            for (int i = 0; i <= node.num_keys; ++i)
                set_parent(node.children[i], node_id);
            write_node(node_id, node);
            for (int i = pos + 1; i < parent.num_keys; ++i)
                parent.entries[i - 1] = parent.entries[i];
            for (int i = pos + 2; i <= parent.num_keys; ++i)
                parent.children[i - 1] = parent.children[i];
            parent.num_keys--;
            write_node(parent_id, parent);
            free_block(right_id);
            if (parent.num_keys < (int)MIN_KEYS_INTERNAL)
                handle_internal_underflow(parent_id);
        } else {
            uint32_t left_id = parent.children[pos - 1];
            Node left;
            read_node(left_id, left);
            left.entries[left.num_keys] = parent.entries[pos - 1];
            memcpy(left.entries + left.num_keys + 1, node.entries, node.num_keys * sizeof(Entry));
            memcpy(left.children + left.num_keys + 1, node.children, (node.num_keys + 1) * sizeof(uint32_t));
            left.num_keys += 1 + node.num_keys;
            for (int i = 0; i <= left.num_keys; ++i)
                set_parent(left.children[i], left_id);
            write_node(left_id, left);
            for (int i = pos; i < parent.num_keys; ++i)
                parent.entries[i - 1] = parent.entries[i];
            for (int i = pos + 1; i <= parent.num_keys; ++i)
                parent.children[i - 1] = parent.children[i];
            parent.num_keys--;
            write_node(parent_id, parent);
            free_block(node_id);
            if (parent.num_keys < (int)MIN_KEYS_INTERNAL)
                handle_internal_underflow(parent_id);
        }
    }

    void init_tree() {
        memset(&header_, 0, sizeof(Header));
        memcpy(header_.magic, "BPTREE01", 8);
        header_.root_block = 1;
        header_.block_count = 2;
        header_.free_list_head = 0;
        write_header();
        Node root;
        memset(&root, 0, sizeof(Node));
        root.is_leaf = 1;
        root.num_keys = 0;
        write_node(1, root);
    }

public:
    BPTree(const string& filename) : filename_(filename), f_(nullptr) {
        f_ = fopen(filename.c_str(), "r+b");
        if (!f_) {
            f_ = fopen(filename.c_str(), "w+b");
            init_tree();
        } else {
            read_header();
            if (memcmp(header_.magic, "BPTREE01", 8) != 0) {
                init_tree();
            }
        }
        cache_.reserve(20000);
        dirty_.reserve(20000);
    }

    ~BPTree() {
        if (f_) {
            for (uint32_t bid : dirty_) {
                auto it = cache_.find(bid);
                if (it != cache_.end()) {
                    fseek(f_, (long)bid * BLOCK_SIZE, SEEK_SET);
                    fwrite(&it->second, sizeof(Node), 1, f_);
                }
            }
            fflush(f_);
            fclose(f_);
        }
    }

    void insert(const string& key, int value) {
        if (header_.root_block == 0) init_tree();
        Entry target;
        set_entry(target, key, value);
        uint32_t leaf_id = find_leaf(target);
        Node leaf;
        read_node(leaf_id, leaf);
        int pos = lower_bound(leaf, target);
        if (pos < leaf.num_keys && cmp_entry(leaf.entries[pos], target) == 0) {
            // duplicate, ignore
            return;
        }
        for (int i = leaf.num_keys - 1; i >= pos; --i)
            leaf.entries[i + 1] = leaf.entries[i];
        leaf.entries[pos] = target;
        leaf.num_keys++;
        write_node(leaf_id, leaf);
        if (leaf.num_keys > (int)MAX_KEYS)
            split_leaf(leaf_id);
    }

    void remove(const string& key, int value) {
        if (header_.root_block == 0) return;
        Entry target;
        set_entry(target, key, value);
        uint32_t leaf_id = find_leaf(target);
        Node leaf;
        read_node(leaf_id, leaf);
        int pos = lower_bound(leaf, target);
        if (pos >= leaf.num_keys || cmp_entry(leaf.entries[pos], target) != 0) return;
        for (int i = pos + 1; i < leaf.num_keys; ++i)
            leaf.entries[i - 1] = leaf.entries[i];
        leaf.num_keys--;
        write_node(leaf_id, leaf);
        if (leaf_id == header_.root_block) {
            if (leaf.num_keys == 0) {
                // keep empty root leaf; nothing to do
            }
            return;
        }
        if (leaf.num_keys < (int)MIN_KEYS_LEAF)
            handle_leaf_underflow(leaf_id);
    }

    vector<int> find(const string& key) {
        vector<int> res;
        if (header_.root_block == 0) return res;
        Entry target;
        set_entry(target, key, INT_MIN);
        uint32_t leaf_id = find_leaf(target);
        Node leaf;
        read_node(leaf_id, leaf);
        while (true) {
            for (int i = 0; i < leaf.num_keys; ++i) {
                if (memcmp(leaf.entries[i].key, target.key, MAX_KEY_LEN) != 0) {
                    // because entries are sorted by key, once key changes we are done
                    if (i == 0 && memcmp(leaf.entries[i].key, target.key, MAX_KEY_LEN) > 0) {
                        // no need to scan further leaves either
                        return res;
                    }
                    continue;
                }
                res.push_back(leaf.entries[i].value);
            }
            uint32_t nxt = leaf.next;
            if (nxt == 0) break;
            read_node(nxt, leaf);
            // check first entry key to avoid unnecessary scanning
            if (leaf.num_keys > 0 && memcmp(leaf.entries[0].key, target.key, MAX_KEY_LEN) != 0)
                break;
        }
        return res;
    }
};

int main() {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);
    BPTree tree("bptree.db");
    int n;
    if (!(cin >> n)) return 0;
    string cmd, key;
    int value;
    for (int i = 0; i < n; ++i) {
        cin >> cmd;
        if (cmd == "insert") {
            cin >> key >> value;
            tree.insert(key, value);
        } else if (cmd == "delete") {
            cin >> key >> value;
            tree.remove(key, value);
        } else if (cmd == "find") {
            cin >> key;
            vector<int> vals = tree.find(key);
            if (vals.empty()) {
                cout << "null\n";
            } else {
                for (size_t j = 0; j < vals.size(); ++j) {
                    if (j) cout << ' ';
                    cout << vals[j];
                }
                cout << "\n";
            }
        }
    }
    return 0;
}
