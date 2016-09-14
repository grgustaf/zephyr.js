// Copyright (c) 2016, Intel Corporation.
#ifdef BUILD_MODULE_AIO
#ifndef QEMU_BUILD
// Zephyr includes
#include <misc/util.h>
#include <string.h>

// ZJS includes
#include "zjs_aio.h"
#include "zjs_callbacks.h"
#include "zjs_ipm.h"
#include "zjs_util.h"

#define ZJS_AIO_TIMEOUT_TICKS                      500

static struct nano_sem aio_sem;

#define MAX_TYPE_LEN 20

typedef struct aio_handle {
    // FIXME: may need event_type here or some type int
    jerry_value_t pin_obj;
    int32_t callback_id;
    double value;
    jerry_value_t jvalue;
} aio_handle_t;

static aio_handle_t *zjs_new_aio_handle()
{
    size_t size = sizeof(aio_handle_t);
    aio_handle_t *handle = task_malloc(size);
    if (handle) {
        memset(handle, 0, size);
    }
    return handle;
}

static void zjs_free_aio_handle(aio_handle_t *handle)
{
    zjs_free(handle);
}

static zjs_ipm_message_t* zjs_aio_alloc_msg()
{
    zjs_ipm_message_t *msg = task_malloc(sizeof(zjs_ipm_message_t));
    if (!msg) {
        PRINT("zjs_aio_alloc_msg: cannot allocate message\n");
        return NULL;
    } else {
        memset(msg, 0, sizeof(zjs_ipm_message_t));
    }

    msg->id = MSG_ID_AIO;
    msg->flags = 0 | MSG_SAFE_TO_FREE_FLAG;
    msg->error_code = ERROR_IPM_NONE;
    return msg;
}

static void zjs_aio_free_msg(zjs_ipm_message_t* msg)
{
    if (!msg)
        return;

    if (msg->flags & MSG_SAFE_TO_FREE_FLAG) {
        task_free(msg);
    } else {
        PRINT("zjs_aio_free_msg: error! do not free message\n");
    }
}

static bool zjs_aio_ipm_send_async(uint32_t type, uint32_t pin, void *data) {
    zjs_ipm_message_t *msg = zjs_aio_alloc_msg();
    msg->type = type;
    msg->user_data = data;
    msg->data.aio.pin = pin;
    msg->data.aio.value = 0;

    int success = zjs_ipm_send(MSG_ID_AIO, msg);
    if (success != 0) {
        PRINT("zjs_aio_ipm_send: failed to send message\n");
        return false;
    }

    zjs_aio_free_msg(msg);
    return true;
}

static bool zjs_aio_ipm_send_sync(zjs_ipm_message_t* send,
                                  zjs_ipm_message_t* result) {
    send->flags |= MSG_SYNC_FLAG;
    send->user_data = (void *)result;
    send->error_code = ERROR_IPM_NONE;

    if (zjs_ipm_send(MSG_ID_AIO, send) != 0) {
        PRINT("zjs_aio_ipm_send_sync: failed to send message\n");
        return false;
    }

    // block until reply or timeout
    if (!nano_sem_take(&aio_sem, ZJS_AIO_TIMEOUT_TICKS)) {
        PRINT("zjs_aio_ipm_send_sync: ipm timed out\n");
        return false;
    }

    return true;
}

static jerry_value_t *zjs_aio_pre_callback(void *h, uint32_t *argc)
{
    aio_handle_t *handle = (aio_handle_t *)h;
    *argc = 1;
    handle->jvalue = jerry_create_number(handle->value);
    return &handle->jvalue;
}

static void zjs_aio_free_callback(void *h, jerry_value_t *ret_val)
{
    aio_handle_t *handle = (aio_handle_t *)h;
    zjs_remove_callback(handle->callback_id);
    zjs_free_aio_handle(handle);
}

static void ipm_msg_receive_callback(void *context, uint32_t id,
                                     volatile void *data)
{
    if (id != MSG_ID_AIO)
        return;

    zjs_ipm_message_t *msg = *(zjs_ipm_message_t **)data;

    if (msg->flags & MSG_SYNC_FLAG) {
        zjs_ipm_message_t *result = (zjs_ipm_message_t*)msg->user_data;
        // synchronous ipm, copy the results
        if (result)
            memcpy(result, msg, sizeof(zjs_ipm_message_t));

        // un-block sync api
        nano_isr_sem_give(&aio_sem);
    } else {
        // asynchronous ipm
        aio_handle_t *handle = (aio_handle_t *)msg->user_data;
        uint32_t pin = msg->data.aio.pin;
        uint32_t pin_value = msg->data.aio.value;

        switch(msg->type) {
        case TYPE_AIO_PIN_READ:
        case TYPE_AIO_PIN_EVENT_VALUE_CHANGE:
            handle->value = (double)pin_value;
            zjs_signal_callback(handle->callback_id);
            break;
        case TYPE_AIO_PIN_SUBSCRIBE:
            PRINT("ipm_msg_receive_callback: subscribed to events on pin %lu\n", pin);
            break;
        case TYPE_AIO_PIN_UNSUBSCRIBE:
            PRINT("ipm_msg_receive_callback: unsubscribed to events on pin %lu\n", pin);
            break;

        default:
            PRINT("ipm_msg_receive_callback: IPM message not handled %lu\n", msg->type);
        }
    }
}

static jerry_value_t zjs_aio_pin_read(const jerry_value_t function_obj,
                                      const jerry_value_t this,
                                      const jerry_value_t argv[],
                                      const jerry_length_t argc)
{
    uint32_t device, pin;
    zjs_obj_get_uint32(this, "device", &device);
    zjs_obj_get_uint32(this, "pin", &pin);

    if (pin < ARC_AIO_MIN || pin > ARC_AIO_MAX) {
        PRINT("PIN: #%lu\n", pin);
        return zjs_error("zjs_aio_pin_read: pin out of range");
    }

    // send IPM message to the ARC side
    zjs_ipm_message_t* reply = zjs_aio_alloc_msg();;
    zjs_ipm_message_t* send = zjs_aio_alloc_msg();

    send->type = TYPE_AIO_PIN_READ;
    send->data.aio.pin = pin;
    bool success = zjs_aio_ipm_send_sync(send, reply);
    zjs_aio_free_msg(send);

    if (!success) {
        zjs_aio_free_msg(reply);
        return zjs_error("zjs_aio_pin_read: ipm message failed or timed out!");
    }

    if (reply->error_code != ERROR_IPM_NONE) {
        PRINT("error code: %lu\n", reply->error_code);
        zjs_aio_free_msg(reply);
        return zjs_error("zjs_aio_pin_read: error received");
    }

    double value = reply->data.aio.value;
    zjs_aio_free_msg(reply);

    return jerry_create_number(value);
}

static jerry_value_t zjs_aio_pin_abort(const jerry_value_t function_obj,
                                       const jerry_value_t this,
                                       const jerry_value_t argv[],
                                       const jerry_length_t argc)
{
    // NO-OP
    return ZJS_UNDEFINED;
}

static jerry_value_t zjs_aio_pin_close(const jerry_value_t function_obj,
                                       const jerry_value_t this,
                                       const jerry_value_t argv[],
                                       const jerry_length_t argc)
{
    // NO-OP
    return ZJS_UNDEFINED;
}

static jerry_value_t zjs_aio_pin_on(const jerry_value_t function_obj,
                                    const jerry_value_t this,
                                    const jerry_value_t argv[],
                                    const jerry_length_t argc)
{
    if (argc < 2 ||
        !jerry_value_is_string(argv[0]) ||
        (!jerry_value_is_object(argv[1]) &&
         !jerry_value_is_null(argv[1]))) {
        return zjs_error("zjs_aio_pin_on: invalid argument");
    }

    uint32_t pin;
    zjs_obj_get_uint32(this, "pin", &pin);

    char event[MAX_TYPE_LEN];
    jerry_value_t arg = argv[0];
    jerry_size_t sz = jerry_get_string_size(arg);
    if (sz >= MAX_TYPE_LEN)
        return zjs_error("zjs_aio_pin_on: event string too long");
    int len = jerry_string_to_char_buffer(arg, (jerry_char_t *)event, sz);
    event[len] = '\0';

    if (strcmp(event, "change")) {
        return zjs_error("zjs_aio_pin_on: unsupported event type");
    }

    static aio_handle_t *handle;
    jerry_get_object_native_handle(this, (uintptr_t * )handle);
    // TODO: clean up old handle?
    if (handle) {
        PRINT("ON HANDLE FOUND\n");
        // FIXME: remove other callback in this file too
        if (jerry_value_is_null(argv[1])) {
            zjs_aio_ipm_send_async(TYPE_AIO_PIN_UNSUBSCRIBE, pin, handle);
            zjs_remove_callback(handle->callback_id);
            jerry_set_object_native_handle(this, 0, NULL);
            zjs_free_aio_handle(handle);
            return ZJS_UNDEFINED;
        }
        zjs_edit_js_func(handle->callback_id, argv[1]);
    }
    else {
        PRINT("ON HANDLE NOT FOUND\n");
        handle = zjs_new_aio_handle();
        PRINT("HANDLE = %p\n", handle);
        if (!handle) {
            return zjs_error("zjs_aio_pin_on: could not allocate handle");
        }
        handle->pin_obj = this;
        handle->callback_id = zjs_add_callback(argv[1], handle,
                                               zjs_aio_pre_callback, NULL);
        zjs_aio_ipm_send_async(TYPE_AIO_PIN_SUBSCRIBE, pin, handle);
    }

    //    item->zjs_cb.call_function = zjs_aio_emit_event;

    return ZJS_UNDEFINED;
}

// Asynchronous Operations
static jerry_value_t zjs_aio_pin_read_async(const jerry_value_t function_obj,
                                            const jerry_value_t this,
                                            const jerry_value_t argv[],
                                            const jerry_length_t argc)
{
    if (argc < 1 || !jerry_value_is_function(argv[0]))
        return zjs_error("zjs_aio_pin_read_async: invalid argument");

    uint32_t device, pin;
    zjs_obj_get_uint32(this, "device", &device);
    zjs_obj_get_uint32(this, "pin", &pin);

    aio_handle_t *handle = zjs_new_aio_handle();
    if (!handle)
        return zjs_error("zjs_aio_pin_read_async: could not allocate handle");
    handle->pin_obj = this;
    handle->callback_id = zjs_add_callback(argv[0], handle,
                                           zjs_aio_pre_callback,
                                           zjs_aio_free_callback);

    //    item->zjs_cb.call_function = zjs_aio_call_function;

    // send IPM message to the ARC side; response will come on an ISR
    zjs_aio_ipm_send_async(TYPE_AIO_PIN_READ, pin, handle);
    return ZJS_UNDEFINED;
}

static jerry_value_t zjs_aio_open(const jerry_value_t function_obj,
                                  const jerry_value_t this,
                                  const jerry_value_t argv[],
                                  const jerry_length_t argc)
{
    if (argc < 1 || !jerry_value_is_object(argv[0]))
        return zjs_error("zjs_aio_open: invalid argument");

    jerry_value_t data = argv[0];

    uint32_t device;
    if (!zjs_obj_get_uint32(data, "device", &device))
        return zjs_error("zjs_aio_open: missing required field (device)");

    uint32_t pin;
    if (!zjs_obj_get_uint32(data, "pin", &pin))
        return zjs_error("zjs_aio_open: missing required field (pin)");

    const int BUFLEN = 32;
    char buffer[BUFLEN];

    if (zjs_obj_get_string(data, "name", buffer, BUFLEN)) {
        buffer[0] = '\0';
    }

    char *name = buffer;
    bool raw = false;
    zjs_obj_get_boolean(data, "raw", &raw);

    // send IPM message to the ARC side
    zjs_ipm_message_t* send = zjs_aio_alloc_msg();
    zjs_ipm_message_t* reply = zjs_aio_alloc_msg();

    send->type = TYPE_AIO_OPEN;
    send->data.aio.pin = pin;
    bool success = zjs_aio_ipm_send_sync(send, reply);
    zjs_aio_free_msg(send);

    if (!success) {
        zjs_aio_free_msg(reply);
        return zjs_error("zjs_aio_open: ipm message failed or timed out!");
    }

    if (reply->error_code != ERROR_IPM_NONE) {
        PRINT("error code: %lu\n", reply->error_code);
        zjs_aio_free_msg(reply);
        return zjs_error("zjs_aio_open: error received");
    }

    zjs_aio_free_msg(send);
    zjs_aio_free_msg(reply);

    // create the AIOPin object
    jerry_value_t pinobj = jerry_create_object();
    zjs_obj_add_function(pinobj, zjs_aio_pin_read, "read");
    zjs_obj_add_function(pinobj, zjs_aio_pin_read_async, "readAsync");
    zjs_obj_add_function(pinobj, zjs_aio_pin_abort, "abort");
    zjs_obj_add_function(pinobj, zjs_aio_pin_close, "close");
    zjs_obj_add_function(pinobj, zjs_aio_pin_on, "on");
    zjs_obj_add_string(pinobj, name, "name");
    zjs_obj_add_number(pinobj, device, "device");
    zjs_obj_add_number(pinobj, pin, "pin");
    zjs_obj_add_boolean(pinobj, raw, "raw");

    return pinobj;
}

jerry_value_t zjs_aio_init()
{
    zjs_ipm_init();
    zjs_ipm_register_callback(MSG_ID_AIO, ipm_msg_receive_callback);

    nano_sem_init(&aio_sem);

    // create global AIO object
    jerry_value_t aio_obj = jerry_create_object();
    zjs_obj_add_function(aio_obj, zjs_aio_open, "open");
    return aio_obj;
}

#endif // QEMU_BUILD
#endif // BUILD_MODULE_AIO
