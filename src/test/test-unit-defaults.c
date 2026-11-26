/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "fileio.h"
#include "manager.h"
#include "path-util.h"
#include "rm-rf.h"
#include "special.h"
#include "tests.h"
#include "tmpfile-util.h"
#include "unit.h"

static const struct {
        const char *name;
        const char *contents;
} units[] = {
        { "network.target",         "[Unit]\n" },
        { "network-online.target",  "[Unit]\nAfter=network.target\n" },
        { "nss-user-lookup.target", "[Unit]\n" },

        { "server.service",      "[Unit]\nAfter=network.target\n[Service]\nExecStart=/bin/true\n" },
        { "local.service",       "[Service]\nExecStart=/bin/true\n" },
        { "wait-online.service", "[Unit]\nAfter=network.target\nBefore=network-online.target\n[Service]\nExecStart=/bin/true\n" },
        { "early.service",       "[Unit]\nDefaultDependencies=no\nAfter=network.target\n[Service]\nExecStart=/bin/true\n" },

        { "ldap-user.service",   "[Service]\nUser=ldapuser\nExecStart=/bin/true\n" },
        { "ldap-group.service",  "[Service]\nSupplementaryGroups=ldapgroup\nExecStart=/bin/true\n" },
        { "root.service",        "[Service]\nUser=root\nGroup=0\nExecStart=/bin/true\n" },
        { "numeric.service",     "[Service]\nUser=4711\nExecStart=/bin/true\n" },
        { "dynamic.service",     "[Service]\nDynamicUser=yes\nUser=dyn\nExecStart=/bin/true\n" },
        { "lookup.service",      "[Unit]\nBefore=nss-user-lookup.target\n[Service]\nUser=sssd\nExecStart=/bin/true\n" },

        { "listen.socket",       "[Unit]\nAfter=network.target\n[Socket]\nListenStream=192.0.2.1:22\n" },
        { "listen.service",      "[Service]\nExecStart=/bin/true\n" },
};

typedef struct Defaults {
        bool wait_network_online;
        bool wait_user_lookup;
} Defaults;

static Unit* load(Manager *m, const char *name) {
        Unit *u = NULL;

        ASSERT_OK(manager_load_unit(m, name, /* path= */ NULL, /* e= */ NULL, &u));
        ASSERT_EQ(u->load_state, UNIT_LOADED);
        return u;
}

static bool after(Manager *m, const char *name, const char *target) {
        Unit *t = ASSERT_NOT_NULL(manager_get_unit(m, target));
        return unit_has_dependency(load(m, name), UNIT_ATOM_AFTER, t);
}

static bool wants(Manager *m, const char *name, const char *target) {
        Unit *t = ASSERT_NOT_NULL(manager_get_unit(m, target));
        return unit_has_dependency(load(m, name), UNIT_ATOM_PULL_IN_START_IGNORED, t);
}

static void test_one(const char *unit_dir, const Defaults *d) {
        _cleanup_(manager_freep) Manager *m = NULL;
        int r;

        log_info("/* %s(wait_network_online=%s, wait_user_lookup=%s) */",
                 __func__, yes_no(d->wait_network_online), yes_no(d->wait_user_lookup));

        ASSERT_OK(setenv_unit_path(unit_dir));

        r = manager_new(RUNTIME_SCOPE_SYSTEM, MANAGER_TEST_RUN_MINIMAL|MANAGER_TEST_DONT_OPEN_EXECUTOR, &m);
        if (manager_errno_skip_test(r))
                return (void) log_tests_skipped_errno(r, "manager_new");
        ASSERT_OK(r);

        m->defaults.wait_network_online = d->wait_network_online;
        m->defaults.wait_user_lookup = d->wait_user_lookup;

        ASSERT_OK(manager_startup(m, /* serialization= */ NULL, /* fds= */ NULL, /* named_listen_fds= */ NULL, /* root= */ NULL));

        /* Make sure the targets exist, so that the checks below are meaningful. */
        load(m, SPECIAL_NETWORK_TARGET);
        load(m, SPECIAL_NETWORK_ONLINE_TARGET);
        load(m, SPECIAL_NSS_USER_LOOKUP_TARGET);

        ASSERT_EQ(after(m, "server.service", SPECIAL_NETWORK_ONLINE_TARGET), d->wait_network_online);
        ASSERT_EQ(wants(m, "server.service", SPECIAL_NETWORK_ONLINE_TARGET), d->wait_network_online);
        ASSERT_FALSE(after(m, "local.service", SPECIAL_NETWORK_ONLINE_TARGET));
        ASSERT_FALSE(after(m, "wait-online.service", SPECIAL_NETWORK_ONLINE_TARGET));
        ASSERT_FALSE(after(m, "early.service", SPECIAL_NETWORK_ONLINE_TARGET));
        ASSERT_FALSE(after(m, "listen.socket", SPECIAL_NETWORK_ONLINE_TARGET));

        ASSERT_EQ(after(m, "ldap-user.service", SPECIAL_NSS_USER_LOOKUP_TARGET), d->wait_user_lookup);
        ASSERT_EQ(after(m, "ldap-group.service", SPECIAL_NSS_USER_LOOKUP_TARGET), d->wait_user_lookup);
        ASSERT_FALSE(wants(m, "ldap-user.service", SPECIAL_NSS_USER_LOOKUP_TARGET));
        ASSERT_FALSE(after(m, "root.service", SPECIAL_NSS_USER_LOOKUP_TARGET));
        ASSERT_FALSE(after(m, "numeric.service", SPECIAL_NSS_USER_LOOKUP_TARGET));
        ASSERT_FALSE(after(m, "dynamic.service", SPECIAL_NSS_USER_LOOKUP_TARGET));
        ASSERT_FALSE(after(m, "lookup.service", SPECIAL_NSS_USER_LOOKUP_TARGET));
}

TEST(unit_defaults) {
        _cleanup_(rm_rf_physical_and_freep) char *unit_dir = NULL, *runtime_dir = NULL;

        ASSERT_OK(mkdtemp_malloc("/tmp/test-unit-defaults-XXXXXX", &unit_dir));
        ASSERT_NOT_NULL(runtime_dir = setup_fake_runtime_dir());

        FOREACH_ELEMENT(i, units) {
                _cleanup_free_ char *p = ASSERT_NOT_NULL(path_join(unit_dir, i->name));
                ASSERT_OK(write_string_file(p, i->contents, WRITE_STRING_FILE_CREATE));
        }

        for (unsigned i = 0; i < 4; i++)
                test_one(unit_dir, &(const Defaults) {
                                .wait_network_online = FLAGS_SET(i, 1U << 0),
                                .wait_user_lookup = FLAGS_SET(i, 1U << 1),
                        });
}

DEFINE_TEST_MAIN(LOG_DEBUG);
