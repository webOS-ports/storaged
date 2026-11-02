/* Copyright (c) 2026 LuneOS project
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests for com.palm.storage/diskmode/enterMSM: storaged itself must refuse
 * to hand /media/internal to the USB host while the device is locked, also
 * when the caller is not the system UI. diskmode.c is compiled in here, with
 * luna-service2, nyx and the signals replaced by fakes that record what it
 * does; a "reply" from com.palm.display is delivered by hand.
 */

#include "../src/diskmode.c"

static int tests_run = 0;
static int tests_failed = 0;

#define CHECK(cond) do { \
    tests_run++; \
    if (!(cond)) { \
        tests_failed++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

/* The opaque bus types, as far as the fakes need them. */
struct LSMessage {
    const char *payload;
    int refs;
    char *reply;
    int replies;
};

static LSHandle *const HANDLE = (LSHandle *) 0x1;

static LSFilterFunc gCallback;
static void *gCallbackCtx;
static char *gCallUri;
static bool gCallFails;
static bool gSetTimeoutFails;
static LSMessageToken gToken;
static int gTimeoutMs;
static int gHostState;
static int gTransitions;

const char *LSMessageGetPayload(LSMessage *message) { return message->payload; }
void LSMessageRef(LSMessage *message) { message->refs++; }
void LSMessageUnref(LSMessage *message) { message->refs--; }

bool LSMessageReply(LSHandle *sh, LSMessage *message, const char *payload, LSError *lserror) {
    g_free(message->reply);
    message->reply = g_strdup(payload);
    message->replies++;
    return true;
}

bool LSCallOneReply(LSHandle *sh, const char *uri, const char *payload, LSFilterFunc callback,
                    void *ctx, LSMessageToken *token, LSError *lserror) {
    if (gCallFails) {
        lserror->func = __func__;
        lserror->message = (char *) "fake failure";
        return false;
    }
    g_free(gCallUri);
    gCallUri = g_strdup(uri);
    gCallback = callback;
    gCallbackCtx = ctx;
    *token = gToken = 42;
    return true;
}

bool LSCallSetTimeout(LSHandle *sh, LSMessageToken token, int timeout_ms, LSError *lserror) {
    if (gSetTimeoutFails) {
        lserror->func = __func__;
        lserror->message = (char *) "fake failure";
        return false;
    }
    CHECK(token == gToken);
    gTimeoutMs = timeout_ms;
    return true;
}

bool LSErrorInit(LSError *error) { memset(error, 0, sizeof(*error)); return true; }
void LSErrorFree(LSError *error) { memset(error, 0, sizeof(*error)); }

bool LSRegisterCategory(LSHandle *sh, const char *category, LSMethod *methods,
                        LSSignal *signals, LSProperty *properties, LSError *error) {
    return true;
}

nyx_error_t nyx_mass_storage_mode_get_state(nyx_device_handle_t handle, nyx_mass_storage_mode_state_t *state) {
    *state = gHostState;
    return NYX_ERROR_NONE;
}

nyx_error_t nyx_mass_storage_mode_set_mode(nyx_device_handle_t handle, nyx_mass_storage_mode_action_t action,
                                           nyx_mass_storage_mode_return_code_t *status) {
    return NYX_ERROR_NONE;
}

nyx_error_t nyx_mass_storage_mode_register_change_callback(nyx_device_handle_t handle,
                                                           nyx_device_callback_function_t cb,
                                                           void *data) {
    return NYX_ERROR_NONE;
}

nyx_device_handle_t GetNyxMassStorageModeDevice(void) { return NULL; }
void disable_lifetime_timer(void) {}
void reset_lifetime_timer(void) {}

void SignalMSMAvailChange(LSHandle *lsh, bool avail) {}
void SignalMSMProgress(LSHandle *lsh, const char *stage, bool forceRequired) {}
void SignalMSMModeChange(LSHandle *lsh, bool entering) {}
void SignalMSMFscking(LSHandle *lsh) {}
void SignalPartitionAvail(LSHandle *lsh, const char *mountPoint, bool avail,
                          bool reformatted, bool fscked) {}

void SignalMSMStatus(LSHandle *lsh, bool entering) {
    if (entering)
        gTransitions++;
}

static void reset(void) {
    g_free(gCallUri);
    gCallUri = NULL;
    gCallback = NULL;
    gCallbackCtx = NULL;
    gCallFails = false;
    gSetTimeoutFails = false;
    gToken = 0;
    gTimeoutMs = 0;
    gHostState = NYX_MASS_STORAGE_MODE_HOST_CONNECTED;
    gTransitions = 0;
    inMSM = false;
    if (sUmountTimerId) {
        g_source_remove(sUmountTimerId);
        sUmountTimerId = 0;
    }
}

static void enter(LSMessage *request, const char *payload) {
    memset(request, 0, sizeof(*request));
    request->payload = payload;
    request->refs = 1;
    handle_enter_mass_storage_mode(HANDLE, request, NULL);
}

/* com.palm.display answers. */
static void answer(const char *payload) {
    LSMessage reply;

    memset(&reply, 0, sizeof(reply));
    reply.payload = payload;
    reply.refs = 1;
    CHECK(gCallback != NULL);
    if (gCallback)
        gCallback(HANDLE, &reply, gCallbackCtx);
}

/* A request asked of a screen in this state is refused, and nothing starts. */
static void check_refused(const char *lockAnswer) {
    LSMessage request;

    reset();
    enter(&request, "{\"user-confirmed\": true}");
    CHECK(request.replies == 0);
    CHECK(gTransitions == 0);
    answer(lockAnswer);
    CHECK(gTransitions == 0);
    CHECK(!inMSM);
    CHECK(sUmountTimerId == 0);
    CHECK(request.replies == 1);
    CHECK(request.reply && strstr(request.reply, "\"result\": false"));
    CHECK(request.reply && strstr(request.reply, "device is locked"));
    CHECK(request.refs == 1);
    g_free(request.reply);
}

static void test_unlocked_enters(void) {
    LSMessage request;

    reset();
    enter(&request, "{\"user-confirmed\": true}");
    CHECK(gCallUri && !strcmp(gCallUri, "luna://com.palm.display/control/lockStatus"));
    CHECK(gTimeoutMs > 0);
    /* Held, and not started, until the lock state is known. */
    CHECK(request.refs == 2);
    CHECK(request.replies == 0);
    CHECK(gTransitions == 0);

    answer("{\"returnValue\":true,\"subscribed\":false,\"lockState\":\"unlocked\"}");
    CHECK(gTransitions == 1);
    CHECK(inMSM);
    CHECK(request.replies == 1);
    CHECK(request.reply && !strcmp(request.reply, "{\"result\": true}"));
    CHECK(request.refs == 1);
    g_free(request.reply);
}

static void test_locked_is_refused(void) {
    check_refused("{\"returnValue\":true,\"subscribed\":false,\"lockState\":\"locked\"}");
}

static void test_dockmode_is_refused(void) {
    check_refused("{\"returnValue\":true,\"subscribed\":false,\"lockState\":\"dockmode\"}");
}

static void test_unknown_lock_state_is_refused(void) {
    check_refused("{\"returnValue\":true}");
    check_refused("{\"returnValue\":true,\"lockState\":true}");
    check_refused("{\"returnValue\":true,\"lockState\":null}");
    check_refused("{\"returnValue\":true,\"lockState\":\"Unlocked\"}");
    check_refused("[\"unlocked\"]");
    check_refused("not json");
    check_refused("");
    check_refused(NULL);
}

static void test_bus_failure_is_refused(void) {
    /* An error reply with no lock state, and the payload luna-service2's
     * callmap.c sends when LSCallSetTimeout's time is up. */
    check_refused("{\"serviceName\":\"com.palm.display\",\"returnValue\":false,"
                  "\"errorCode\":-1,\"errorText\":\"Service does not exist\"}");
    check_refused("{\"returnValue\":false,\"errorCode\":-1,"
                  "\"errorText\":\"Timeout expired for call.\"}");
}

static void test_query_that_cannot_be_sent_is_refused(void) {
    LSMessage request;

    reset();
    gCallFails = true;
    enter(&request, "{\"user-confirmed\": true}");
    CHECK(gTransitions == 0);
    CHECK(request.replies == 1);
    CHECK(request.reply && strstr(request.reply, "device is locked"));
    CHECK(request.refs == 1);
    g_free(request.reply);
}

static void test_timeout_that_cannot_be_set_still_waits(void) {
    LSMessage request;

    reset();
    gSetTimeoutFails = true;
    enter(&request, "{\"user-confirmed\": true}");
    CHECK(request.replies == 0);
    answer("{\"returnValue\":true,\"lockState\":\"locked\"}");
    CHECK(gTransitions == 0);
    CHECK(request.replies == 1);
    g_free(request.reply);
}

static void test_cable_pulled_while_asking(void) {
    LSMessage request;

    reset();
    enter(&request, "{\"user-confirmed\": true}");
    gHostState = 0;
    answer("{\"returnValue\":true,\"lockState\":\"unlocked\"}");
    CHECK(gTransitions == 0);
    CHECK(request.replies == 1);
    CHECK(request.reply && !strcmp(request.reply, "{\"result\": true}"));
    g_free(request.reply);
}

static void test_not_confirmed_asks_nothing(void) {
    LSMessage request;

    reset();
    enter(&request, "{\"user-confirmed\": false}");
    CHECK(gCallUri == NULL);
    CHECK(gTransitions == 0);
    CHECK(request.replies == 1);
    CHECK(request.reply && !strcmp(request.reply, "{\"result\": true}"));
    CHECK(request.refs == 1);
    g_free(request.reply);

    reset();
    enter(&request, "not json");
    CHECK(gCallUri == NULL);
    CHECK(request.replies == 1);
    CHECK(request.reply && strstr(request.reply, "\"result\": false"));
    g_free(request.reply);
}

int main(void) {
    test_unlocked_enters();
    test_locked_is_refused();
    test_dockmode_is_refused();
    test_unknown_lock_state_is_refused();
    test_bus_failure_is_refused();
    test_query_that_cannot_be_sent_is_refused();
    test_timeout_that_cannot_be_set_still_waits();
    test_cable_pulled_while_asking();
    test_not_confirmed_asks_nothing();

    reset();
    printf("%s: %d checks, %d failed\n",
           tests_failed ? "FAIL" : "PASS", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
