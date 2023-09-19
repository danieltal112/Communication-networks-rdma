#ifndef EX3_HASHTABLE_H
#define EX3_HASHTABLE_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TABLE_SIZE 16
#define MAX_OVERLOAD_FACTOR 0.75
#define MIN_OVERLOAD_FACTOR 0.25
#define GROWTH_FACTOR 2

typedef struct KeyValuePair {
    char* key;
    char* value;
    char* locked_value;
    int sem_lock;
    struct KeyValuePair* next;
} KeyValuePair;


typedef struct {
    KeyValuePair** entries;
    size_t capacity;
    size_t size;
} HashTable;

HashTable* initializeHashTable();
int hashTable_set(const char* key, char* value, HashTable* table);
void hashTable_get(const char* key, char** value, HashTable* table);
int hashTable_set_lock(const char* key, HashTable* table);
int hashTable_release_lock(const char* key, HashTable* table);
void release_db(HashTable* table);
#endif //EX3_HASHTABLE_H
