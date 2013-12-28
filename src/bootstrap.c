#include "internal.h"
#include "bucketconfig/clconfig.h"

struct lcb_bootstrap_st {
    clconfig_listener listener;
    lcb_t parent;
    lcb_timer_t timer;
    int active;
};

#define LOGARGS(instance, lvl) \
    &instance->settings, "bootstrap", LCB_LOG_##lvl, __FILE__, __LINE__

static void config_callback(clconfig_info *info, clconfig_listener *listener)
{
    struct lcb_bootstrap_st *bootstrap = (struct lcb_bootstrap_st *)listener;
    lcb_t instance = bootstrap->parent;
    instance->last_error = LCB_SUCCESS;
    bootstrap->active = 0;

    if (bootstrap->timer) {
        lcb_timer_destroy(instance, bootstrap->timer);
        bootstrap->timer = NULL;
    }

    lcb_log(LOGARGS(instance, DEBUG), "Instance configured!");
    lcb_update_vbconfig(instance, info);
    lcb_maybe_breakout(instance);
}

static void initial_timeout(lcb_timer_t timer, lcb_t instance,
                            const void *unused)
{
    instance->last_error = lcb_confmon_last_error(instance->confmon);
    if (instance->last_error == LCB_SUCCESS) {
        instance->last_error = LCB_ETIMEDOUT;
    }

    instance->bootstrap->active = 0;

    lcb_error_handler(instance, instance->last_error,
                      "Failed to bootstrap in time");

    lcb_log(LOGARGS(instance, DEBUG),
            "Failed to bootstrap in time. 0x%x", instance->last_error);

    lcb_maybe_breakout(instance);
    (void)unused;
    (void)timer;
}

static void async_refresh(lcb_timer_t timer, lcb_t unused,
                          const void *cookie)
{
    /** Get the best configuration and run stuff.. */
    struct lcb_bootstrap_st *bs = (struct lcb_bootstrap_st *)cookie;
    clconfig_info *info;

    info = lcb_confmon_get_config(bs->parent->confmon);
    config_callback(info, &bs->listener);

    (void)unused;
    (void)timer;
}

static void async_step_callback(clconfig_info *info,
                                clconfig_listener *listener)
{
    lcb_error_t err;
    struct lcb_bootstrap_st *bs = (struct lcb_bootstrap_st *)listener;

    lcb_log(LOGARGS(bs->parent, INFO), "Got async step callback..");

    bs->timer = lcb_async_create(bs->parent->settings.io,
                                 bs, async_refresh, &err);

    (void)info;
}

static lcb_error_t bootstrap_common(lcb_t instance, int initial)
{
    struct lcb_bootstrap_st *bs = instance->bootstrap;

    if (bs && bs->active) {
        return LCB_SUCCESS;
    }

    if (!bs) {
        bs = calloc(1, sizeof(*instance->bootstrap));
        if (!bs) {
            return LCB_CLIENT_ENOMEM;
        }

        instance->bootstrap = bs;
        bs->parent = instance;
        lcb_confmon_add_listener(instance->confmon, &bs->listener);
    }

    bs->active = 1;

    if (initial) {
        lcb_error_t err;
        bs->listener.callback = config_callback;
        bs->timer = lcb_timer_create(instance, NULL,
                                     instance->settings.config_timeout, 0,
                                     initial_timeout, &err);
        if (err != LCB_SUCCESS) {
            return err;
        }

    } else {
        /** No initial timer */
        bs->listener.callback = async_step_callback;
    }

    return lcb_confmon_start(instance->confmon);
}

lcb_error_t lcb_bootstrap_initial(lcb_t instance)
{
    lcb_confmon_prepare(instance->confmon);
    return bootstrap_common(instance, 1);
}

lcb_error_t lcb_bootstrap_refresh(lcb_t instance)
{
    return bootstrap_common(instance, 0);
}

void lcb_bootstrap_errcount_incr(lcb_t instance)
{
    instance->weird_things++;
    if (instance->weird_things == instance->settings.weird_things_threshold) {
        instance->weird_things = 0;
        lcb_bootstrap_refresh(instance);
    }
}

void lcb_bootstrap_destroy(lcb_t instance)
{
    struct lcb_bootstrap_st *bs = instance->bootstrap;
    if (!bs) {
        return;
    }

    if (bs->timer) {
        lcb_timer_destroy(instance, bs->timer);
    }

    lcb_confmon_remove_listener(instance->confmon, &bs->listener);
    free(bs);
    instance->bootstrap = NULL;
}
