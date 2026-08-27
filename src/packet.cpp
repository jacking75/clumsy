#include <stdlib.h>
#include <string.h>
#include "common.h"

static PacketNode headNode = {0}, tailNode = {0};
// `extern` is required in C++: a namespace-scope const object would otherwise
// get internal linkage, and divert.cpp's `extern PacketNode * const head;`
// declaration would not resolve.
extern PacketNode * const head = &headNode, * const tail = &tailNode;

void initPacketNodeList() {
    if (head->next == NULL && tail->prev == NULL) {
        // first time initializing
        head->next = tail;
        tail->prev = head;
    } else {
        // have used before, then check node is empty
        assert(isListEmpty());
    }
}

// TODO  using malloc in the loop is not good for performance
//       just not sure I can write a better memory allocator
PacketNode* createNode(char* buf, UINT len, const PacketMeta *meta,
                       const void *backendMeta) {
    PacketNode *newNode = (PacketNode*)malloc(sizeof(PacketNode));
    newNode->packet = (char*)malloc(len);
    memcpy(newNode->packet, buf, len);
    newNode->packetLen = len;
    if (meta) {
        newNode->meta = *meta;
    } else {
        memset(&(newNode->meta), 0, sizeof(newNode->meta));
    }
    // The blob is opaque here on purpose: only the capture backend knows what
    // is in it, and it is always copied whole.
    if (backendMeta) {
        memcpy(newNode->backend.raw, backendMeta, PACKET_BACKEND_META_SIZE);
    } else {
        memset(newNode->backend.raw, 0, PACKET_BACKEND_META_SIZE);
    }
    newNode->timestamp = 0;
    newNode->next = newNode->prev = NULL;
    return newNode;
}

PacketNode* cloneNode(const PacketNode *src) {
    PacketNode *copy = createNode(src->packet, src->packetLen, &(src->meta), NULL);
    packetBackendPrepareClone(src, copy);
    return copy;
}

void freeNode(PacketNode *node) {
    assert((node != head) && (node != tail));
    // Give the capture backend a chance to settle its own bookkeeping for a
    // packet that is going away without being sent (Phase 4.2).
    packetBackendOnFree(node);
    free(node->packet);
    free(node);
}

PacketNode* popNode(PacketNode *node) {
    assert((node != head) && (node != tail));
    node->prev->next = node->next;
    node->next->prev = node->prev;
    return node;
}

PacketNode* insertAfter(PacketNode *node, PacketNode *target) {
    assert(node && node != head && node != tail && target != tail);
    node->prev = target;
    node->next = target->next;
    target->next->prev = node;
    target->next = node;
    return node;
}

PacketNode* insertBefore(PacketNode *node, PacketNode *target) {
    assert(node && node != head && node != tail && target != head);
    node->next = target;
    node->prev = target->prev;
    target->prev->next = node;
    target->prev = node;
    return node;
}

PacketNode* appendNode(PacketNode *node) {
    return insertBefore(node, tail);
}

short isListEmpty() {
    return head->next == tail;
}