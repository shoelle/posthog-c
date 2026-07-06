#include "mock_transport.h"
#include "ph_internal.h"

#include <stdlib.h>
#include <string.h>

#define MOCK_MAX 128

static ph_mutex g_lock;
static int g_inited;
static char *g_bodies[MOCK_MAX];
static int g_count;
static int g_status = 200;
static char *g_flags_response;

static void ensure(void) {
    if (!g_inited) {
        ph_mutex_init(&g_lock);
        g_inited = 1;
    }
}

static int mock_send(void *self, const char *url, const char *body, size_t len,
                     int timeout_ms) {
    int s;
    (void)self;
    (void)url;
    (void)timeout_ms;
    ensure();
    ph_mutex_lock(&g_lock);
    if (g_count < MOCK_MAX) {
        char *copy = (char *)malloc(len + 1);
        if (copy) {
            memcpy(copy, body, len);
            copy[len] = '\0';
            g_bodies[g_count++] = copy;
        }
    }
    s = g_status;
    ph_mutex_unlock(&g_lock);
    return s;
}

static int mock_fetch(void *self, const char *url, const char *body, size_t len,
                      int timeout_ms, char *out, size_t out_cap) {
    int s;
    (void)self;
    (void)url;
    (void)body;
    (void)len;
    (void)timeout_ms;
    ensure();
    ph_mutex_lock(&g_lock);
    if (out && out_cap > 0) {
        const char *r = g_flags_response ? g_flags_response : "{\"flags\":{}}";
        size_t n = strlen(r);
        if (n >= out_cap) n = out_cap - 1;
        memcpy(out, r, n);
        out[n] = '\0';
    }
    s = g_status;
    ph_mutex_unlock(&g_lock);
    return s;
}

void mock_set_flags_response(const char *json) {
    ensure();
    ph_mutex_lock(&g_lock);
    free(g_flags_response);
    g_flags_response = NULL;
    if (json) {
        g_flags_response = (char *)malloc(strlen(json) + 1);
        if (g_flags_response) strcpy(g_flags_response, json);
    }
    ph_mutex_unlock(&g_lock);
}

void mock_install(void) {
    ph_transport t;
    ensure();
    t.send = mock_send;
    t.fetch = mock_fetch;
    t.destroy = NULL;
    t.self = NULL;
    ph__set_transport(&t);
}

void mock_reset(void) {
    int i;
    ensure();
    ph_mutex_lock(&g_lock);
    for (i = 0; i < g_count; i++) {
        free(g_bodies[i]);
        g_bodies[i] = NULL;
    }
    g_count = 0;
    g_status = 200;
    free(g_flags_response);
    g_flags_response = NULL;
    ph_mutex_unlock(&g_lock);
}

void mock_set_status(int status) {
    ensure();
    ph_mutex_lock(&g_lock);
    g_status = status;
    ph_mutex_unlock(&g_lock);
}

int mock_batch_count(void) {
    int n;
    ensure();
    ph_mutex_lock(&g_lock);
    n = g_count;
    ph_mutex_unlock(&g_lock);
    return n;
}

const char *mock_batch(int i) {
    const char *p = NULL;
    ensure();
    ph_mutex_lock(&g_lock);
    if (i >= 0 && i < g_count) p = g_bodies[i];
    ph_mutex_unlock(&g_lock);
    return p;
}
