#include "utils/hashset.h"



PG_FUNCTION_INFO_V1(hashset_in);
Datum hashset_in(PG_FUNCTION_ARGS)
{
    ereport(WARNING, (errmsg("hashset_in not implemented")));
    PG_RETURN_NULL();
}
PG_FUNCTION_INFO_V1(hashset_out);
Datum hashset_out(PG_FUNCTION_ARGS)
{
    ereport(WARNING, (errmsg("hashset_out not implemented")));
    PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(hashset_send);
Datum hashset_send(PG_FUNCTION_ARGS)
{
    ereport(WARNING, (errmsg("hashset_send not implemented")));
    PG_RETURN_NULL();
}


PG_FUNCTION_INFO_V1(hashset_recv);
Datum hashset_recv(PG_FUNCTION_ARGS)
{
    ereport(WARNING, (errmsg("hashset_recv not implemented")));
    PG_RETURN_NULL();
}


PG_FUNCTION_INFO_V1(hashset_contains);
Datum hashset_contains(PG_FUNCTION_ARGS)
{
     PG_RETURN_BOOL(true);
//    hashset *set = AG_GET_ARG_HASHSET_P(0);
// ereport(WARNING, (errmsg("contains check %i", searchSerializedHashSetInBuffer(set->data, set->data_size, AG_GETARG_GRAPHID(1)))));
  //  PG_RETURN_BOOL(searchSerializedHashSetInBuffer(set->data, set->data_size, AG_GETARG_GRAPHID(1)));
}


size_t getHashSetSize(const HashSetValue* set) {
    size_t header_size = sizeof(int) * 2;
    size_t index_table_size = (size_t)set->capacity * sizeof(SerializedBucketInfo);
    size_t key_data_size = (size_t)set->size * sizeof(graphid);
    return header_size + index_table_size + key_data_size;
}

/**
 * @brief Serializes a hash set into a buffer, preserving the bucket structure for faster searching.
 * @param set The hash set to serialize.
 * @param buffer_size A pointer to a size_t variable to store the size of the created buffer.
 * @return A pointer to the newly allocated buffer, or NULL on failure. The caller is responsible for freeing this buffer.
 */
void serializeHashSetToBuffer(const HashSetValue* set, char* buffer, size_t buffer_size)
{
    size_t header_size = sizeof(int) * 2;
    size_t index_table_size = (size_t)set->capacity * sizeof(SerializedBucketInfo);
    size_t key_data_size = (size_t)set->size * sizeof(graphid);




    char* ptr = buffer;

    // 1. Write metadata header
    memcpy(ptr, &set->capacity, sizeof(int));
    ptr += sizeof(int);
    memcpy(ptr, &set->size, sizeof(int));
    ptr += sizeof(int);

    // 2. Write the bucket index table and the key data
    SerializedBucketInfo* index_table = (SerializedBucketInfo*)ptr;
    ptr += index_table_size; // Move pointer past the index table for now

    char* key_data_ptr = ptr;
    uint32_t current_key_offset = 0;

    for (int i = 0; i < set->capacity; i++) {
        uint32_t key_count_in_bucket = 0;
        HashSetNode* currentNode = set->buckets[i];

        index_table[i].offset = current_key_offset;

        while (currentNode != NULL) {
            memcpy(key_data_ptr, &currentNode->key, sizeof(graphid));
            key_data_ptr += sizeof(graphid);
            key_count_in_bucket++;
            currentNode = currentNode->next;
        }
        index_table[i].count = key_count_in_bucket;
        current_key_offset += key_count_in_bucket * sizeof(graphid);
    }
}
/**
 * @brief Deserializes a hash set from a memory buffer.
 * @param buffer A pointer to the buffer containing the serialized data.
 * @param buffer_size The size of the buffer.
 * @return A pointer to the newly created HashSet, or NULL on failure.
 */
HashSetValue* deserializeHashSetFromBuffer(const char* buffer, size_t buffer_size) {
    if (buffer == NULL || buffer_size < sizeof(int) * 2) return NULL;

    const char* ptr = buffer;
    int capacity, size;

    // 1. Read metadata
    memcpy(&capacity, ptr, sizeof(int));
    ptr += sizeof(int);
    memcpy(&size, ptr, sizeof(int));
    ptr += sizeof(int);

    // 2. Create a new hash set
    HashSetValue* set = createHashSet(capacity);
    if (set == NULL) return NULL;

    // 3. Find the start of the key data and insert keys
    ptr += (size_t)capacity * sizeof(SerializedBucketInfo);
    for (int i = 0; i < size; i++) {
        graphid key;
        memcpy(&key, ptr, sizeof(graphid));
        ptr += sizeof(graphid);
        insert(set, key);
    }

    return set;
}

/**
 * @brief Searches for a key in a structured serialized buffer, leveraging the hash structure.
 * @param buffer A pointer to the buffer containing the serialized data.
 * @param buffer_size The size of the buffer.
 * @param key The key to search for.
 * @return true if the key is found, false otherwise.
 */
bool searchSerializedHashSetInBuffer(const char* buffer, size_t buffer_size, graphid key) {


    const char* ptr = buffer;
    int capacity;

    // 1. Read capacity to use the hash function
    memcpy(&capacity, ptr, sizeof(int));


    // 2. Calculate the target bucket index
    int bucket_index = hashFunction(key, capacity);

    // 3. Find the metadata for that bucket
    const char* index_table_ptr = buffer + sizeof(int) * 2;
    SerializedBucketInfo bucket_info;
    memcpy(&bucket_info, index_table_ptr + (size_t)bucket_index * sizeof(SerializedBucketInfo), sizeof(SerializedBucketInfo));

    // 4. Find the start of the key data for that bucket
    const char* key_data_start = index_table_ptr + (size_t)capacity * sizeof(SerializedBucketInfo) + bucket_info.offset;

    // 5. Scan only the keys within that bucket
    for (uint32_t i = 0; i < bucket_info.count; i++) {
        graphid current_key;
        memcpy(&current_key, key_data_start + i * sizeof(graphid), sizeof(graphid));
        if (current_key == key) {
            return true;
        }
    }

    return false;
}

/**
 * @brief Creates a new HashSetNode.
 * @param key The graphid key for the new node.
 * @return A pointer to the newly created HashSetNode.
 */
HashSetNode* createHashSetNode(graphid key) {
    HashSetNode* newNode = (HashSetNode*)palloc(sizeof(HashSetNode));
    newNode->key = key;
    newNode->next = NULL;
    return newNode;
}

/**
 * @brief Creates and initializes a new HashSetValue.
 * @param capacity The initial number of buckets for the hash set.
 * @return A pointer to the newly created HashSetValue, or NULL if capacity is invalid.
 */
HashSetValue* createHashSet(int capacity) {
    HashSetValue* set = (HashSetValue*)palloc(sizeof(HashSetValue));

    set->capacity = capacity;
    set->size = 0;
    set->buckets = (HashSetNode**)palloc0(capacity * sizeof(HashSetNode*));

    return set;
}

/**
 * @brief A simple hash function to map a key to a bucket index.
 * @param key The key to hash.
 * @param capacity The capacity of the hash set.
 * @return The calculated index for the key.
 */
int hashFunction(graphid key, int capacity) {
    return key % capacity;
}

/**
 * @brief Inserts a key into the hash set.
 * @param set A pointer to the HashSetValue.
 * @param key The key to insert.
 */
void insert(HashSetValue* set, graphid key) {
    if (set == NULL) return;

    int index = hashFunction(key, set->capacity);
    HashSetNode* currentNode = set->buckets[index];

    while (currentNode != NULL) {
        if (currentNode->key == key) {
            return; 
        }
        currentNode = currentNode->next;
    }

    HashSetNode* newNode = createHashSetNode(key);
    newNode->next = set->buckets[index];
    set->buckets[index] = newNode;
    set->size++;
}

/**
 * @brief Checks if a key exists in the hash set.
 * @param set A pointer to the HashSetValue.
 * @param key The key to search for.
 * @return true if the key is found, false otherwise.
 */
bool contains(HashSetValue* set, graphid key) {
    if (set == NULL) return false;

    int index = hashFunction(key, set->capacity);
    HashSetNode* currentNode = set->buckets[index];

    while (currentNode != NULL) {
        if (currentNode->key == key) {
            return true;
        }
        currentNode = currentNode->next;
    }

    return false;
}

/**
 * @brief Removes a key from the hash set.
 * @param set A pointer to the HashSetValue.
 * @param key The key to remove.
 * @return true if the key was found and removed, false otherwise.
 */
bool removeElement(HashSetValue* set, graphid key) {
    if (set == NULL) return false;

    int index = hashFunction(key, set->capacity);
    HashSetNode* currentNode = set->buckets[index];
    HashSetNode* prevNode = NULL;

    while (currentNode != NULL) {
        if (currentNode->key == key) {
            if (prevNode == NULL) {
                set->buckets[index] = currentNode->next;
            } else {
                prevNode->next = currentNode->next;
            }
            pfree(currentNode);
            set->size--;
            return true;
        }
        prevNode = currentNode;
        currentNode = currentNode->next;
    }

    return false;
}

/**
 * @brief Frees all memory associated with the hash set.
 * @param set A pointer to the HashSetValue to destroy.
 */
void destroyHashSet(HashSetValue* set) {
    if (set == NULL) return;

    for (int i = 0; i < set->capacity; i++) {
        HashSetNode* currentNode = set->buckets[i];
        while (currentNode != NULL) {
            HashSetNode* temp = currentNode;
            currentNode = currentNode->next;

            pfree(temp);
        }
    }
    pfree(set->buckets);
    pfree(set);
}
