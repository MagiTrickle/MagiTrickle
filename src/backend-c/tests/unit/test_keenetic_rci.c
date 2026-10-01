/* Ports src/backend/internal/interfaces/keenetic_router_specific_test.go
 * (TestKeeneticRouterSpecificAPIGetIfaceAliasesUsesBatchRCI), plus the
 * edge cases that test left implicit in Go's decoder behaviour.
 *
 * The Go test span a httptest server; here the transport is exercised
 * separately from the parsing, so these cover the parse/merge core that
 * actually encodes the RCI contract -- request shape, positional
 * response matching, and the description/interface-name/skip rules. */
#include "greatest.h"

#include <stdlib.h>
#include <string.h>

#include "magitrickle/keenetic_rci.h"

/* The two payloads the Go test's stub server returned. */
static const char *const LIST_JSON =
    "{\"Wireguard0\":{\"description\":\"Home VPN\"},"
    "\"Wireguard1\":{\"interface-name\":\"Backup VPN\"}}";

static const char *const NAMES_JSON =
    "[{\"show\":{\"interface\":{\"system-name\":\"nwg0\"}}},"
    "{\"show\":{\"interface\":{\"system-name\":\"nwg1\"}}}]";

static const mt_kn_iface_meta_t *find_meta(const mt_kn_iface_meta_t *metas, size_t n,
                                           const char *id)
{
    for (size_t i = 0; i < n; i++) {
        if (strcmp(metas[i].id, id) == 0) {
            return &metas[i];
        }
    }
    return NULL;
}

TEST parse_interface_list_reads_both_label_fields(void)
{
    mt_kn_iface_meta_t *metas = NULL;
    size_t n = 0;
    ASSERT_EQ(MT_OK, mt_kn_parse_interface_list(LIST_JSON, &metas, &n));
    ASSERT_EQ(2, n);

    const mt_kn_iface_meta_t *wg0 = find_meta(metas, n, "Wireguard0");
    const mt_kn_iface_meta_t *wg1 = find_meta(metas, n, "Wireguard1");
    ASSERT(wg0 != NULL);
    ASSERT(wg1 != NULL);
    ASSERT_STR_EQ("Home VPN", wg0->description);
    ASSERT_STR_EQ("", wg0->interface_name);
    ASSERT_STR_EQ("", wg1->description);
    ASSERT_STR_EQ("Backup VPN", wg1->interface_name);

    free(metas);
    PASS();
}

/* Go decoded each map value into a struct and `continue`d on error, so a
 * non-object value drops the entry rather than failing the whole list. */
TEST parse_interface_list_skips_non_object_entries(void)
{
    mt_kn_iface_meta_t *metas = NULL;
    size_t n = 0;
    ASSERT_EQ(MT_OK, mt_kn_parse_interface_list(
                         "{\"Good\":{\"description\":\"ok\"},\"Bad\":\"nope\",\"Also\":123}",
                         &metas, &n));
    ASSERT_EQ(1, n);
    ASSERT_STR_EQ("Good", metas[0].id);

    free(metas);
    PASS();
}

TEST parse_interface_list_rejects_malformed(void)
{
    mt_kn_iface_meta_t *metas = NULL;
    size_t n = 0;
    ASSERT_EQ(MT_ERR_PROTO, mt_kn_parse_interface_list("{oops", &metas, &n));
    ASSERT_EQ(MT_ERR_PROTO, mt_kn_parse_interface_list("[\"an array\"]", &metas, &n));
    PASS();
}

/* The Go test asserted one POST carrying one element per interface, each
 * with name/details/system-name. */
TEST build_system_name_request_is_one_batch(void)
{
    mt_kn_iface_meta_t metas[2] = {0};
    snprintf(metas[0].id, sizeof(metas[0].id), "%s", "Wireguard0");
    snprintf(metas[1].id, sizeof(metas[1].id), "%s", "Wireguard1");

    char *body = mt_kn_build_system_name_request(metas, 2);
    ASSERT(body != NULL);
    ASSERT_STR_EQ(
        "[{\"show\":{\"interface\":{\"name\":\"Wireguard0\",\"details\":\"yes\","
        "\"system-name\":\"yes\"}}},"
        "{\"show\":{\"interface\":{\"name\":\"Wireguard1\",\"details\":\"yes\","
        "\"system-name\":\"yes\"}}}]",
        body);

    free(body);
    PASS();
}

/* RCI does not echo the interface id, so responses are matched by
 * position -- the single most breakable part of this contract. */
TEST parse_system_names_matches_positionally(void)
{
    mt_kn_iface_meta_t metas[2] = {0};
    snprintf(metas[0].id, sizeof(metas[0].id), "%s", "Wireguard0");
    snprintf(metas[1].id, sizeof(metas[1].id), "%s", "Wireguard1");

    ASSERT_EQ(MT_OK, mt_kn_parse_system_names(NAMES_JSON, metas, 2));
    ASSERT_STR_EQ("nwg0", metas[0].system_name);
    ASSERT_STR_EQ("nwg1", metas[1].system_name);
    PASS();
}

/* Go bounds-checked both directions: extra elements break out of the
 * loop, missing ones simply leave the entry unset. */
TEST parse_system_names_tolerates_length_mismatch(void)
{
    mt_kn_iface_meta_t few[1] = {0};
    ASSERT_EQ(MT_OK, mt_kn_parse_system_names(NAMES_JSON, few, 1));
    ASSERT_STR_EQ("nwg0", few[0].system_name);

    mt_kn_iface_meta_t many[3] = {0};
    ASSERT_EQ(MT_OK, mt_kn_parse_system_names(NAMES_JSON, many, 3));
    ASSERT_STR_EQ("nwg0", many[0].system_name);
    ASSERT_STR_EQ("nwg1", many[1].system_name);
    ASSERT_STR_EQ("", many[2].system_name);
    PASS();
}

/* End to end over the parse/merge core: the Go test's exact assertions. */
TEST aliases_prefer_description_then_interface_name(void)
{
    mt_kn_iface_meta_t *metas = NULL;
    size_t n = 0;
    ASSERT_EQ(MT_OK, mt_kn_parse_interface_list(LIST_JSON, &metas, &n));
    ASSERT_EQ(2, n);

    /* Feed system names in whatever order the list parse produced, so
     * the test does not depend on JSON member ordering. */
    for (size_t i = 0; i < n; i++) {
        if (strcmp(metas[i].id, "Wireguard0") == 0) {
            snprintf(metas[i].system_name, sizeof(metas[i].system_name), "%s", "nwg0");
        } else {
            snprintf(metas[i].system_name, sizeof(metas[i].system_name), "%s", "nwg1");
        }
    }

    mt_kn_aliases_t aliases = {0};
    ASSERT_EQ(MT_OK, mt_kn_build_aliases(metas, n, &aliases));
    ASSERT_EQ(2, aliases.n);
    ASSERT_STR_EQ("Home VPN", mt_kn_aliases_lookup(&aliases, "nwg0"));
    ASSERT_STR_EQ("Backup VPN", mt_kn_aliases_lookup(&aliases, "nwg1"));
    ASSERT(mt_kn_aliases_lookup(&aliases, "br0") == NULL);

    mt_kn_aliases_free(&aliases);
    free(metas);
    PASS();
}

TEST aliases_skip_empty_and_redundant(void)
{
    mt_kn_iface_meta_t metas[4] = {0};

    /* No system name -> nothing to key the alias on. */
    snprintf(metas[0].description, sizeof(metas[0].description), "%s", "Orphan");

    /* No label at all. */
    snprintf(metas[1].system_name, sizeof(metas[1].system_name), "%s", "eth0");

    /* Label identical to the system name adds nothing for the user. */
    snprintf(metas[2].description, sizeof(metas[2].description), "%s", "eth1");
    snprintf(metas[2].system_name, sizeof(metas[2].system_name), "%s", "eth1");

    /* Whitespace-only label is empty once trimmed. */
    snprintf(metas[3].description, sizeof(metas[3].description), "%s", "   ");
    snprintf(metas[3].system_name, sizeof(metas[3].system_name), "%s", "eth2");

    mt_kn_aliases_t aliases = {0};
    ASSERT_EQ(MT_OK, mt_kn_build_aliases(metas, 4, &aliases));
    ASSERT_EQ(0, aliases.n);
    ASSERT(mt_kn_aliases_lookup(&aliases, "eth0") == NULL);

    mt_kn_aliases_free(&aliases);
    PASS();
}

/* Trimming happens at parse time, mirroring Go's strings.TrimSpace on
 * both the label and the system name. */
TEST labels_and_system_names_are_trimmed(void)
{
    mt_kn_iface_meta_t *metas = NULL;
    size_t n = 0;
    ASSERT_EQ(MT_OK, mt_kn_parse_interface_list(
                         "{\"Wg\":{\"description\":\"  Padded VPN \\t\"}}", &metas, &n));
    ASSERT_EQ(1, n);
    ASSERT_STR_EQ("Padded VPN", metas[0].description);

    ASSERT_EQ(MT_OK, mt_kn_parse_system_names(
                         "[{\"show\":{\"interface\":{\"system-name\":\"  nwg0  \"}}}]", metas, 1));
    ASSERT_STR_EQ("nwg0", metas[0].system_name);

    mt_kn_aliases_t aliases = {0};
    ASSERT_EQ(MT_OK, mt_kn_build_aliases(metas, 1, &aliases));
    ASSERT_EQ(1, aliases.n);
    ASSERT_STR_EQ("Padded VPN", mt_kn_aliases_lookup(&aliases, "nwg0"));

    mt_kn_aliases_free(&aliases);
    free(metas);
    PASS();
}

/* An empty interface list must not produce a batch call at all -- Go
 * short-circuited on len(interfaceIDs) == 0. */
TEST empty_list_yields_no_aliases(void)
{
    mt_kn_iface_meta_t *metas = NULL;
    size_t n = 0;
    ASSERT_EQ(MT_OK, mt_kn_parse_interface_list("{}", &metas, &n));
    ASSERT_EQ(0, n);

    mt_kn_aliases_t aliases = {0};
    ASSERT_EQ(MT_OK, mt_kn_build_aliases(metas, n, &aliases));
    ASSERT_EQ(0, aliases.n);
    ASSERT(aliases.items == NULL);

    mt_kn_aliases_free(&aliases);
    free(metas);
    PASS();
}

/* Off Keenetic the lookup is a no-op that never touches the network,
 * matching Go's DummyRouterSpecificAPI under !entware_kn. On Keenetic
 * builds RCI is genuinely contacted; 127.0.0.1:79 is not listening in
 * the test environment, so only assert it stays well-behaved. */
TEST platform_default_is_empty_and_safe(void)
{
    mt_kn_aliases_t aliases = {0};
    mt_err_t err = mt_kn_get_iface_aliases(&aliases);
#ifdef MT_ENTWARE_KN
    (void)err;
#else
    ASSERT_EQ(MT_OK, err);
#endif
    ASSERT_EQ(0, aliases.n);
    ASSERT(aliases.items == NULL);
    ASSERT(mt_kn_aliases_lookup(&aliases, "br0") == NULL);

    mt_kn_aliases_free(&aliases);
    /* Idempotent free. */
    mt_kn_aliases_free(&aliases);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(parse_interface_list_reads_both_label_fields);
    RUN_TEST(parse_interface_list_skips_non_object_entries);
    RUN_TEST(parse_interface_list_rejects_malformed);
    RUN_TEST(build_system_name_request_is_one_batch);
    RUN_TEST(parse_system_names_matches_positionally);
    RUN_TEST(parse_system_names_tolerates_length_mismatch);
    RUN_TEST(aliases_prefer_description_then_interface_name);
    RUN_TEST(aliases_skip_empty_and_redundant);
    RUN_TEST(labels_and_system_names_are_trimmed);
    RUN_TEST(empty_list_yields_no_aliases);
    RUN_TEST(platform_default_is_empty_and_safe);
    GREATEST_MAIN_END();
}
