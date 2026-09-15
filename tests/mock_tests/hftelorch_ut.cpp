#include "mock_sai_capability_wrap.h"
// Pre-include standard headers before exposing private members to the fixtures.
#include <sstream>
#include <memory>
#include <functional>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <set>
#define private public
#define protected public
#include "high_frequency_telemetry/hftelorch.h"
#undef protected
#undef private
#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "high_frequency_telemetry/hftelutils.h"
#include "schema.h"
#include "json.h"
#include "notifications.h"
#include "sai_serialize.h"
#include <gtest/gtest.h>

extern sai_switch_api_t *sai_switch_api;
extern sai_tam_api_t *sai_tam_api;
extern redisReply *mockReply;

namespace hftelorch_test
{
    using namespace std;
    using hftelorch_sai_wrap_ut::HFTelSaiHookGuard;

    namespace constructor_ut
    {
        sai_switch_api_t *pold_sai_switch_api = nullptr;
        sai_switch_api_t ut_sai_switch_api{};

        sai_status_t _ut_stub_sai_set_switch_attribute(
            _In_ sai_object_id_t switch_id,
            _In_ const sai_attribute_t *attr)
        {
            if (attr->id == SAI_SWITCH_ATTR_TAM_TEL_TYPE_CONFIG_CHANGE_NOTIFY)
            {
                return SAI_STATUS_FAILURE;
            }

            return pold_sai_switch_api->set_switch_attribute(switch_id, attr);
        }

        void hookSaiSwitchApi()
        {
            ut_sai_switch_api = *sai_switch_api;
            pold_sai_switch_api = sai_switch_api;
            ut_sai_switch_api.set_switch_attribute = _ut_stub_sai_set_switch_attribute;
            sai_switch_api = &ut_sai_switch_api;
        }

        void unhookSaiSwitchApi()
        {
            sai_switch_api = pold_sai_switch_api;
            pold_sai_switch_api = nullptr;
        }
    }

    class HFTelOrchIsSupportedTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            map<string, string> profile = {
                {"SAI_VS_SWITCH_TYPE", "SAI_VS_SWITCH_TYPE_BCM56850"},
                {"KV_DEVICE_MAC_ADDRESS", "20:03:04:05:06:00"},
            };

            ASSERT_EQ(ut_helper::initSaiApi(profile), SAI_STATUS_SUCCESS);

            sai_attribute_t attr{};
            attr.id = SAI_SWITCH_ATTR_INIT_SWITCH;
            attr.value.booldata = true;

            ASSERT_EQ(sai_switch_api->create_switch(&gSwitchId, 1, &attr), SAI_STATUS_SUCCESS);
        }

        void TearDown() override
        {
            hftelorch_sai_wrap_ut::setSaiHookNone();

            ASSERT_EQ(sai_switch_api->remove_switch(gSwitchId), SAI_STATUS_SUCCESS);
            gSwitchId = SAI_NULL_OBJECT_ID;

            ASSERT_EQ(ut_helper::uninitSaiApi(), SAI_STATUS_SUCCESS);
        }
    };

    TEST_F(HFTelOrchIsSupportedTest, IsSupportedHFTel_with_virtual_switch)
    {
        bool supported = HFTelOrch::isSupportedHFTel(gSwitchId);
        (void)supported;
    }

    TEST_F(HFTelOrchIsSupportedTest, IsSupportedHFTel_null_switch_id)
    {
        EXPECT_FALSE(HFTelOrch::isSupportedHFTel(SAI_NULL_OBJECT_ID));
    }

    /*
     * Forces sai_query_stats_st_capability to fail (not SUCCESS / BUFFER_OVERFLOW).
     * Covers: "Streaming stats not supported, HFTel disabled"
     */
    TEST_F(HFTelOrchIsSupportedTest, IsSupportedHFTel_negative_streaming_stats_unsupported)
    {
        HFTelSaiHookGuard guard(hftelorch_sai_wrap_ut::setSaiHookStatsStFail);
        EXPECT_FALSE(HFTelOrch::isSupportedHFTel(gSwitchId));
    }

    /*
     * First sai_query_attribute_capability in the probe fails.
     * Covers: "HFTel: %s capability query failed, HFTel disabled"
     */
    TEST_F(HFTelOrchIsSupportedTest, IsSupportedHFTel_negative_attribute_capability_query_failed)
    {
        HFTelSaiHookGuard guard(hftelorch_sai_wrap_ut::setSaiHookAttributeCapabilityQueryFail);
        EXPECT_FALSE(HFTelOrch::isSupportedHFTel(gSwitchId));
    }

    /*
     * sai_query_attribute_capability succeeds for TAM_COLLECTOR but reports
     * create_implemented == false.
     * Covers: "HFTel: %s create not supported, HFTel disabled"
     */
    TEST_F(HFTelOrchIsSupportedTest, IsSupportedHFTel_negative_collector_create_not_supported)
    {
        HFTelSaiHookGuard guard(hftelorch_sai_wrap_ut::setSaiHookCollectorCreateNotImplemented);
        EXPECT_FALSE(HFTelOrch::isSupportedHFTel(gSwitchId));
    }

    /*
     * Past collector checks, SAI_SWITCH_ATTR_TAM_TEL_TYPE_CONFIG_CHANGE_NOTIFY reports
     * set not implemented.
     * Covers: "HFTel: %s set not supported, HFTel disabled"
     */
    TEST_F(HFTelOrchIsSupportedTest, IsSupportedHFTel_negative_switch_notify_set_not_supported)
    {
        HFTelSaiHookGuard guard(hftelorch_sai_wrap_ut::setSaiHookSwitchNotifySetNotImplemented);
        EXPECT_FALSE(HFTelOrch::isSupportedHFTel(gSwitchId));
    }

    /*
     * All checks pass — happy path through the entire function.
     * The AllSupported hook makes attribute capability return all-supported,
     * then real sai_query_attribute_enum_values_capability handles enum checks.
     * Covers: the full attribute loop, enum loop, and "return true" at the end.
     */
    TEST_F(HFTelOrchIsSupportedTest, IsSupportedHFTel_positive_all_supported)
    {
        HFTelSaiHookGuard guard(hftelorch_sai_wrap_ut::setSaiHookAllSupported);
        // VS SAI may or may not support all enum values, so we just exercise
        // the code path without asserting the result.
        bool supported = HFTelOrch::isSupportedHFTel(gSwitchId);
        (void)supported;
    }

    /*
     * Fixture for the SAI_TAM_TEL_TYPE_ATTR_MODE capability matrix tests.
     * Inherits the base setUp/tearDown and additionally installs the
     * AllSupported attribute-capability hook so isSupportedHFTel reaches the
     * mode probe instead of bouncing on an earlier capability check. The
     * mode hook is installed per-test via SaiHookGuard.
     */
    class HFTelOrchModeTest : public HFTelOrchIsSupportedTest
    {
    protected:
        void SetUp() override
        {
            HFTelOrchIsSupportedTest::SetUp();
            hftelorch_sai_wrap_ut::setSaiHookAllSupported();
        }
        // TearDown is inherited; SaiHookGuard::~SaiHookGuard() also clears the
        // attribute-capability hook via setSaiHookNone() between tests.
    };

    /*
     * SAI_TAM_TEL_TYPE_ATTR_MODE advertises SINGLE only.
     */
    TEST_F(HFTelOrchModeTest, IsSupportedHFTel_mode_single_only)
    {
        HFTelSaiHookGuard guard(hftelorch_sai_wrap_ut::setSaiHookModeAdvertisedSingleOnly);
        EXPECT_TRUE(HFTelOrch::isSupportedHFTel(gSwitchId));
    }

    /*
     * SAI_TAM_TEL_TYPE_ATTR_MODE advertises MIXED only.
     */
    TEST_F(HFTelOrchModeTest, IsSupportedHFTel_mode_mixed_only)
    {
        HFTelSaiHookGuard guard(hftelorch_sai_wrap_ut::setSaiHookModeAdvertisedMixedOnly);
        EXPECT_TRUE(HFTelOrch::isSupportedHFTel(gSwitchId));
    }

    /*
     * SAI_TAM_TEL_TYPE_ATTR_MODE advertises both SINGLE and MIXED.
     */
    TEST_F(HFTelOrchModeTest, IsSupportedHFTel_mode_both)
    {
        HFTelSaiHookGuard guard(hftelorch_sai_wrap_ut::setSaiHookModeAdvertisedBoth);
        EXPECT_TRUE(HFTelOrch::isSupportedHFTel(gSwitchId));
    }

    /*
     * SAI_TAM_TEL_TYPE_ATTR_MODE advertises neither SINGLE nor MIXED.
     * Covers: "HFTel: neither SAI_TAM_TEL_TYPE_MODE_SINGLE_TYPE nor
     *          SAI_TAM_TEL_TYPE_MODE_MIXED_TYPE advertised, HFTel disabled"
     */
    TEST_F(HFTelOrchModeTest, IsSupportedHFTel_mode_neither)
    {
        HFTelSaiHookGuard guard(hftelorch_sai_wrap_ut::setSaiHookModeAdvertisedNeither);
        EXPECT_FALSE(HFTelOrch::isSupportedHFTel(gSwitchId));
    }

    /*
     * SAI_TAM_TEL_TYPE_ATTR_MODE advertises MIXED only, but none of the three
     * SWITCH_ENABLE_*_STATS attributes are implemented on TAM_TEL_TYPE. MIXED
     * can never support any category, so it must be treated the same as "not
     * advertised": with SINGLE also unavailable, HFTel is disabled.
     * Covers: querySupportedTelTypeModes' mixed_supported downgrade when
     * tel_type_supported_categories ends up empty.
     */
    TEST_F(HFTelOrchModeTest, IsSupportedHFTel_mixedOnly_noCategoriesSupported_disablesHft)
    {
        HFTelSaiHookGuard guard(hftelorch_sai_wrap_ut::setSaiHookModeAdvertisedMixedOnly);
        hftelorch_sai_wrap_ut::setSaiHookMixedEnableAttrsAllUnsupported();
        EXPECT_FALSE(HFTelOrch::isSupportedHFTel(gSwitchId));
    }

    /*
     * Same as above, but both modes are advertised. The SWITCH_ENABLE_*_STATS
     * probe is not MIXED-specific - SINGLE mode needs the same attributes, one
     * at a time, so with none implemented neither mode can bind any object
     * type. HFTel must be disabled, not silently fall back to a SINGLE_TYPE
     * that would fail on every category too.
     */
    TEST_F(HFTelOrchModeTest, IsSupportedHFTel_bothAdvertised_noCategoriesSupported_disablesHft)
    {
        HFTelSaiHookGuard guard(hftelorch_sai_wrap_ut::setSaiHookModeAdvertisedBoth);
        hftelorch_sai_wrap_ut::setSaiHookMixedEnableAttrsAllUnsupported();
        EXPECT_FALSE(HFTelOrch::isSupportedHFTel(gSwitchId));
    }

    /*
     * SAI_TAM_TEL_TYPE_ATTR_MODE advertises SINGLE only, but none of the three
     * SWITCH_ENABLE_*_STATS attributes are implemented on TAM_TEL_TYPE. SINGLE
     * can never bind any object type either, so it must be treated the same as
     * "not advertised": with MIXED also unavailable, HFTel is disabled.
     */
    TEST_F(HFTelOrchModeTest, IsSupportedHFTel_singleOnly_noCategoriesSupported_disablesHft)
    {
        HFTelSaiHookGuard guard(hftelorch_sai_wrap_ut::setSaiHookModeAdvertisedSingleOnly);
        hftelorch_sai_wrap_ut::setSaiHookMixedEnableAttrsAllUnsupported();
        EXPECT_FALSE(HFTelOrch::isSupportedHFTel(gSwitchId));
    }

    /*
     * SAI_TAM_TEL_TYPE_ATTR_MODE advertises MIXED only, and only the MMU_STATS
     * enable attribute is unimplemented (PORT_STATS / OUTPUT_QUEUE_STATS are
     * fine). MIXED must stay usable for the supported categories, so HFTel
     * remains enabled.
     */
    TEST_F(HFTelOrchModeTest, IsSupportedHFTel_mixedOnly_partialCategoriesSupported_staysEnabled)
    {
        HFTelSaiHookGuard guard(hftelorch_sai_wrap_ut::setSaiHookModeAdvertisedMixedOnly);
        hftelorch_sai_wrap_ut::setSaiHookMixedEnableAttrsMmuUnsupported();
        EXPECT_TRUE(HFTelOrch::isSupportedHFTel(gSwitchId));
    }

    /*
     * Same partial-support probe result, but with SINGLE_TYPE advertised
     * instead of MIXED_TYPE: SINGLE only needs one attribute per object type,
     * so PORT_STATS / OUTPUT_QUEUE_STATS being implemented is enough to stay
     * enabled even with MMU_STATS unimplemented.
     */
    TEST_F(HFTelOrchModeTest, IsSupportedHFTel_singleOnly_partialCategoriesSupported_staysEnabled)
    {
        HFTelSaiHookGuard guard(hftelorch_sai_wrap_ut::setSaiHookModeAdvertisedSingleOnly);
        hftelorch_sai_wrap_ut::setSaiHookMixedEnableAttrsMmuUnsupported();
        EXPECT_TRUE(HFTelOrch::isSupportedHFTel(gSwitchId));
    }

    class HFTelOrchConstructorTest : public ::testing::Test
    {
    protected:
        shared_ptr<swss::DBConnector> m_config_db;
        shared_ptr<swss::DBConnector> m_state_db;

        void SetUp() override
        {
            map<string, string> profile = {
                {"SAI_VS_SWITCH_TYPE", "SAI_VS_SWITCH_TYPE_BCM56850"},
                {"KV_DEVICE_MAC_ADDRESS", "20:03:04:05:06:00"},
            };

            ASSERT_EQ(ut_helper::initSaiApi(profile), SAI_STATUS_SUCCESS);

            sai_attribute_t attr{};
            attr.id = SAI_SWITCH_ATTR_INIT_SWITCH;
            attr.value.booldata = true;

            ASSERT_EQ(sai_switch_api->create_switch(&gSwitchId, 1, &attr), SAI_STATUS_SUCCESS);

            m_config_db = make_shared<swss::DBConnector>("CONFIG_DB", 0);
            m_state_db = make_shared<swss::DBConnector>("STATE_DB", 0);

            constructor_ut::hookSaiSwitchApi();
        }

        void TearDown() override
        {
            constructor_ut::unhookSaiSwitchApi();

            ASSERT_EQ(sai_switch_api->remove_switch(gSwitchId), SAI_STATUS_SUCCESS);
            gSwitchId = SAI_NULL_OBJECT_ID;

            ASSERT_EQ(ut_helper::uninitSaiApi(), SAI_STATUS_SUCCESS);
        }
    };

    /*
     * Forces set_switch_attribute for TAM_TEL_TYPE_CONFIG_CHANGE_NOTIFY to fail.
     * Covers constructor error cleanup: delete notifier and nullptr consumer.
     */
    TEST_F(HFTelOrchConstructorTest, ConstructorFailsWhenTamNotifySetFails)
    {
        const vector<string> stel_tables = {
            CFG_HIGH_FREQUENCY_TELEMETRY_PROFILE_TABLE_NAME,
            CFG_HIGH_FREQUENCY_TELEMETRY_GROUP_TABLE_NAME,
        };

        EXPECT_THROW(
            {
                HFTelOrch orch(m_config_db.get(), m_state_db.get(), stel_tables);
                (void)orch;
            },
            runtime_error);
    }

    class HFTelOrchShutdownTest : public ::testing::Test
    {
    protected:
        shared_ptr<swss::DBConnector> m_config_db;
        shared_ptr<swss::DBConnector> m_state_db;

        void SetUp() override
        {
            map<string, string> profile = {
                {"SAI_VS_SWITCH_TYPE", "SAI_VS_SWITCH_TYPE_BCM56850"},
                {"KV_DEVICE_MAC_ADDRESS", "20:03:04:05:06:00"},
            };

            ASSERT_EQ(ut_helper::initSaiApi(profile), SAI_STATUS_SUCCESS);

            sai_attribute_t attr{};
            attr.id = SAI_SWITCH_ATTR_INIT_SWITCH;
            attr.value.booldata = true;

            ASSERT_EQ(sai_switch_api->create_switch(&gSwitchId, 1, &attr), SAI_STATUS_SUCCESS);

            m_config_db = make_shared<swss::DBConnector>("CONFIG_DB", 0);
            m_state_db = make_shared<swss::DBConnector>("STATE_DB", 0);
        }

        void TearDown() override
        {
            ASSERT_EQ(sai_switch_api->remove_switch(gSwitchId), SAI_STATUS_SUCCESS);
            gSwitchId = SAI_NULL_OBJECT_ID;

            ASSERT_EQ(ut_helper::uninitSaiApi(), SAI_STATUS_SUCCESS);
        }
    };

    /*
     * Successful ctor then dtor: Notifier/Executor owns the ASIC NotificationConsumer.
     * Regression for double-delete on shutdown (shared_ptr member + ~Executor).
     */
    TEST_F(HFTelOrchShutdownTest, DestructorDoesNotDoubleDeleteNotificationConsumer)
    {
        const vector<string> stel_tables = {
            CFG_HIGH_FREQUENCY_TELEMETRY_PROFILE_TABLE_NAME,
            CFG_HIGH_FREQUENCY_TELEMETRY_GROUP_TABLE_NAME,
        };

        auto orch = make_unique<HFTelOrch>(m_config_db.get(), m_state_db.get(), stel_tables);
        orch.reset();
    }

    class HFTelSessionOwnerTest : public HFTelOrchShutdownTest
    {
    protected:
        unique_ptr<HFTelOrch> orch;
        shared_ptr<HFTelProfile> profile;
        sai_tam_api_t tam_api{};
        sai_tam_api_t *saved_tam_api = nullptr;
        static vector<uint8_t> template_data;
        static vector<pair<sai_object_id_t, int32_t>> transitions;
        static sai_object_id_t next_counter;
        const string name = "hft_owner_test";

        void SetUp() override
        {
            HFTelOrchShutdownTest::SetUp();
            saved_tam_api = sai_tam_api;
            tam_api = *sai_tam_api;
            tam_api.get_tam_tel_type_attribute = [](sai_object_id_t, uint32_t count, sai_attribute_t *attrs) -> sai_status_t {
                if (count != 1 || attrs->id != SAI_TAM_TEL_TYPE_ATTR_IPFIX_TEMPLATES)
                    return SAI_STATUS_INVALID_PARAMETER;
                auto &list = attrs->value.u8list;
                if (list.count < template_data.size())
                {
                    list.count = static_cast<uint32_t>(template_data.size());
                    return SAI_STATUS_BUFFER_OVERFLOW;
                }
                copy(template_data.begin(), template_data.end(), list.list);
                list.count = static_cast<uint32_t>(template_data.size());
                return SAI_STATUS_SUCCESS;
            };
            tam_api.set_tam_tel_type_attribute = [](sai_object_id_t oid, const sai_attribute_t *attr) -> sai_status_t {
                transitions.emplace_back(oid, attr->value.s32);
                return SAI_STATUS_SUCCESS;
            };
            tam_api.create_tam_counter_subscription = [](sai_object_id_t *oid, sai_object_id_t,
                                                        uint32_t, const sai_attribute_t *) -> sai_status_t {
                *oid = ++next_counter;
                return SAI_STATUS_SUCCESS;
            };
            tam_api.remove_tam_counter_subscription = [](sai_object_id_t) -> sai_status_t { return SAI_STATUS_SUCCESS; };
            sai_tam_api = &tam_api;
            transitions.clear();
            template_data.clear();
        }

        void TearDown() override
        {
            profile.reset();
            orch.reset();
            sai_tam_api = saved_tam_api;
            hftelorch_sai_wrap_ut::setSaiHookNone();
            swss::Table table(m_state_db.get(), STATE_HIGH_FREQUENCY_TELEMETRY_SESSION_TABLE_NAME);
            for (const auto &suffix : {"MIXED", "PORT", "QUEUE"})
                table.del(name + "|" + suffix);
            HFTelOrchShutdownTest::TearDown();
        }

        void createOrch(bool mixed = true)
        {
            hftelorch_sai_wrap_ut::setSaiHookAllSupported();
            if (mixed)
                hftelorch_sai_wrap_ut::setSaiHookModeAdvertisedMixedOnly();
            else
                hftelorch_sai_wrap_ut::setSaiHookModeAdvertisedSingleOnly();
            orch = make_unique<HFTelOrch>(m_config_db.get(), m_state_db.get(), vector<string>{
                CFG_HIGH_FREQUENCY_TELEMETRY_PROFILE_TABLE_NAME,
                CFG_HIGH_FREQUENCY_TELEMETRY_GROUP_TABLE_NAME});
            ASSERT_EQ(orch->profileTableSet(name, {{"stream_state", "enabled"}}), task_success);
            profile = orch->tryGetProfile(name);
            ASSERT_NE(profile, nullptr);
            ASSERT_EQ(profile->isMixedTypeMode(), mixed);
            // Keep hardware creation out of these producer tests; exercise the
            // real profile/group lifecycle using tracked telemetry-type handles.
            for (const auto type : {SAI_OBJECT_TYPE_PORT, SAI_OBJECT_TYPE_QUEUE})
            {
                const auto key = profile->mapKey(type);
                if (profile->m_sai_tam_tel_type_objs.count(key))
                    continue;
                auto guard = make_shared<sai_object_id_t>(0x700 + key);
                profile->m_sai_tam_tel_type_objs[key] = guard;
                profile->m_sai_tam_tel_type_states[guard] = SAI_TAM_TEL_TYPE_STATE_STOP_STREAM;
                profile->m_sai_tam_report_objs[key] = make_shared<sai_object_id_t>(0x800 + key);
            }
            orch->m_counter_name_cache[SAI_OBJECT_TYPE_PORT] = {{"Ethernet0", 0x101}, {"Ethernet4", 0x102}, {"Ethernet8", 0x103}};
            orch->m_counter_name_cache[SAI_OBJECT_TYPE_QUEUE] = {{"Ethernet0|0", 0x201}, {"Ethernet4|0", 0x202}};
        }

        void setGroup(const string &group, const string &names)
        {
            const auto type = HFTelUtils::group_name_to_sai_type(group);
            const auto stats = sai_metadata_get_object_type_info(type)->statenum;
            const auto stat = type == SAI_OBJECT_TYPE_PORT
                ? static_cast<sai_stat_id_t>(SAI_PORT_STAT_IF_IN_OCTETS)
                : static_cast<sai_stat_id_t>(SAI_QUEUE_STAT_PACKETS);
            string counter;
            for (size_t i = 0; i < stats->valuescount; ++i)
                if (stats->values[i] == stat)
                    counter = stats->valuesshortnames[i];
            ASSERT_FALSE(counter.empty());
            ASSERT_EQ(orch->groupTableSet(name, group, {
                {"object_names", names}, {"object_counters", counter}}), task_success);
        }

        void notify(sai_object_id_t oid)
        {
            const auto message = swss::JSon::buildJson({{
                SAI_SWITCH_NOTIFICATION_NAME_TAM_TEL_TYPE_CONFIG_CHANGE, sai_serialize_object_id(oid)}});
            redisReply payload{};
            payload.type = REDIS_REPLY_STRING;
            payload.str = const_cast<char *>(message.c_str());
            payload.len = message.size();
            redisReply *elements[] = {nullptr, nullptr, &payload};
            redisReply reply{};
            reply.type = REDIS_REPLY_ARRAY;
            reply.elements = 3;
            reply.element = elements;
            mockReply = &reply;
            orch->m_asic_notification_consumer->readData();
            mockReply = nullptr;
            orch->doTask(*orch->m_asic_notification_consumer);
        }

        void ready(sai_object_type_t type, uint16_t generation = 300)
        {
            template_data.clear();
            // A complete binary IPFIX snapshot, one template per object, with
            // embedded NULs and enterprise labels from the configured groups.
            for (const auto &group : profile->m_groups)
            {
                if (!profile->isMixedTypeMode() && group.first != type)
                    continue;
                for (const auto &object : group.second.getObjects())
                {
                    const auto id = static_cast<uint16_t>(generation + object.second);
                    vector<uint8_t> bytes = {
                        0, 10, 0, 36, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7,
                        0, 2, 0, 20, static_cast<uint8_t>(id >> 8), static_cast<uint8_t>(id),
                        0, 2, 1, 69, 0, 8,
                        static_cast<uint8_t>(0x80 | (object.second >> 8)), static_cast<uint8_t>(object.second), 0, 8,
                        0, static_cast<uint8_t>(group.first), 0, static_cast<uint8_t>(*group.second.getStatsIDs().begin())};
                    template_data.insert(template_data.end(), bytes.begin(), bytes.end());
                }
            }
            notify(*profile->m_sai_tam_tel_type_objs.at(profile->mapKey(type)));
        }

        map<string, string> row(const string &suffix)
        {
            vector<swss::FieldValueTuple> values;
            EXPECT_TRUE(orch->m_state_telemetry_session.get(name + "|" + suffix, values));
            return {values.begin(), values.end()};
        }

        void expectKeys(const set<string> &suffixes)
        {
            vector<string> keys;
            orch->m_state_telemetry_session.getKeys(keys);
            set<string> actual;
            for (const auto &key : keys)
                if (key.compare(0, name.size() + 1, name + "|") == 0)
                    actual.insert(key.substr(name.size() + 1));
            EXPECT_EQ(actual, suffixes);
        }

        void expectSnapshot(const string &suffix, const string &status, const vector<sai_object_type_t> &types)
        {
            const auto values = row(suffix);
            ASSERT_EQ(values.size(), 5u);
            EXPECT_EQ(values.at("stream_status"), status);
            EXPECT_EQ(values.at("session_type"), "ipfix");
            EXPECT_EQ(values.at("session_config"), string(template_data.begin(), template_data.end()));
            map<string, string> expected;
            for (const auto type : types)
                for (const auto &object : profile->m_groups.at(type).getObjects())
                    EXPECT_TRUE(expected.emplace(to_string(object.second), object.first).second);
            istringstream names(values.at("object_names")), ids(values.at("object_ids"));
            string object_name, id;
            map<string, string> actual;
            while (getline(names, object_name, ','))
            {
                ASSERT_TRUE(static_cast<bool>(getline(ids, id, ',')));
                EXPECT_TRUE(actual.emplace(id, object_name).second);
            }
            EXPECT_FALSE(static_cast<bool>(getline(ids, id, ',')));
            EXPECT_EQ(actual, expected);
        }

        void twoGroups()
        {
            setGroup("PORT", "Ethernet0,Ethernet4");
            ready(SAI_OBJECT_TYPE_PORT);
            setGroup("QUEUE", "Ethernet0|0,Ethernet4|0");
            ready(SAI_OBJECT_TYPE_QUEUE, 400);
        }
    };

    vector<uint8_t> HFTelSessionOwnerTest::template_data;
    vector<pair<sai_object_id_t, int32_t>> HFTelSessionOwnerTest::transitions;
    sai_object_id_t HFTelSessionOwnerTest::next_counter = 0x900;

    TEST_F(HFTelSessionOwnerTest, MixedPublishesCompleteSnapshotUnderOneStableOwner)
    {
        createOrch();
        expectKeys({}); // Profile enable alone must not create a partial row.
        setGroup("PORT", "Ethernet0,Ethernet4");
        expectKeys({});
        ready(SAI_OBJECT_TYPE_PORT);
        const auto original = row("MIXED");
        setGroup("QUEUE", "Ethernet0|0,Ethernet4|0");
        EXPECT_EQ(row("MIXED"), original); // No metadata-only intermediate snapshot.
        ready(SAI_OBJECT_TYPE_QUEUE, 400);
        expectKeys({"MIXED"});
        expectSnapshot("MIXED", "enabled", {SAI_OBJECT_TYPE_PORT, SAI_OBJECT_TYPE_QUEUE});
        const auto complete = row("MIXED");
        transitions.clear();
        setGroup("QUEUE", "Ethernet0|0,Ethernet4|0");
        EXPECT_TRUE(transitions.empty());
        EXPECT_EQ(row("MIXED"), complete);
        template_data = {1, 2, 3};
        notify(*profile->m_sai_tam_tel_type_objs.at(SAI_OBJECT_TYPE_NULL));
        EXPECT_EQ(row("MIXED"), complete); // Duplicate callback cannot replace it.
    }

    TEST_F(HFTelSessionOwnerTest, MixedDisableEnablePreservesCompleteMetadataAndRestartsOnce)
    {
        createOrch();
        twoGroups();
        const auto original = row("MIXED");
        transitions.clear();
        ASSERT_EQ(orch->profileTableSet(name, {{"stream_state", "disabled"}}), task_success);
        expectSnapshot("MIXED", "disabled", {SAI_OBJECT_TYPE_PORT, SAI_OBJECT_TYPE_QUEUE});
        ASSERT_EQ(orch->profileTableSet(name, {{"stream_state", "enabled"}}), task_success);
        EXPECT_EQ(row("MIXED"), original);
        expectKeys({"MIXED"});
        ASSERT_EQ(transitions.size(), 2u);
        EXPECT_EQ(transitions[0].second, SAI_TAM_TEL_TYPE_STATE_STOP_STREAM);
        EXPECT_EQ(transitions[1].second, SAI_TAM_TEL_TYPE_STATE_START_STREAM);
        EXPECT_EQ(transitions[0].first, transitions[1].first);
    }

    TEST_F(HFTelSessionOwnerTest, MixedDeleteGroupRegeneratesSurvivorAndLastDeleteRemovesOwner)
    {
        createOrch();
        twoGroups();
        const auto original = row("MIXED");
        const auto oid = *profile->m_sai_tam_tel_type_objs.at(SAI_OBJECT_TYPE_NULL);
        transitions.clear();
        ASSERT_EQ(orch->groupTableDel(name, "PORT"), task_success);
        EXPECT_EQ(row("MIXED"), original);
        EXPECT_EQ(profile->getStreamState(SAI_OBJECT_TYPE_QUEUE), SAI_TAM_TEL_TYPE_STATE_CREATE_CONFIG);
        EXPECT_EQ(*profile->m_sai_tam_tel_type_objs.at(SAI_OBJECT_TYPE_NULL), oid);
        ASSERT_EQ(transitions.size(), 2u);
        EXPECT_EQ(transitions[0].second, SAI_TAM_TEL_TYPE_STATE_STOP_STREAM);
        EXPECT_EQ(transitions[1].second, SAI_TAM_TEL_TYPE_STATE_CREATE_CONFIG);
        ready(SAI_OBJECT_TYPE_QUEUE, 500);
        expectKeys({"MIXED"});
        expectSnapshot("MIXED", "enabled", {SAI_OBJECT_TYPE_QUEUE});
        const auto survivor = row("MIXED");
        transitions.clear();
        ASSERT_EQ(orch->groupTableDel(name, "PORT"), task_success);
        EXPECT_EQ(row("MIXED"), survivor);
        EXPECT_TRUE(transitions.empty());
        ASSERT_EQ(orch->groupTableDel(name, "QUEUE"), task_success);
        expectKeys({});
        EXPECT_TRUE(profile->m_sai_tam_tel_type_objs.empty());
        EXPECT_TRUE(profile->m_sai_tam_report_objs.empty());
        EXPECT_TRUE(profile->m_sai_tam_tel_type_templates.empty());
        notify(oid); // Late callback for the removed shared hardware object.
        expectKeys({});
        ASSERT_EQ(orch->profileTableDel(name), task_success);
        EXPECT_EQ(orch->tryGetProfile(name), nullptr);
    }

    TEST_F(HFTelSessionOwnerTest, MixedReconfigurationWaitsForCompleteTemplateBeforeEnable)
    {
        createOrch();
        twoGroups();
        const auto original = row("MIXED");
        // Ethernet12 is not in the name cache yet: do not restart on old templates.
        setGroup("PORT", "Ethernet8,Ethernet12");
        EXPECT_EQ(row("MIXED"), original);
        EXPECT_TRUE(profile->m_sai_tam_tel_type_templates.empty());
        ASSERT_EQ(orch->profileTableSet(name, {{"stream_state", "enabled"}}), task_success);
        EXPECT_EQ(profile->getStreamState(SAI_OBJECT_TYPE_PORT), SAI_TAM_TEL_TYPE_STATE_STOP_STREAM);
        auto disabled = original;
        disabled["stream_status"] = "disabled";
        EXPECT_EQ(row("MIXED"), disabled);
        CounterNameMapUpdater::Message message;
        message.m_table_name = COUNTERS_PORT_NAME_MAP;
        message.m_operation = CounterNameMapUpdater::SET;
        message.m_counter_name = "Ethernet12";
        message.m_oid = 0x104;
        orch->locallyNotify(message);
        EXPECT_EQ(profile->getStreamState(SAI_OBJECT_TYPE_QUEUE), SAI_TAM_TEL_TYPE_STATE_CREATE_CONFIG);
        ready(SAI_OBJECT_TYPE_PORT, 600);
        expectKeys({"MIXED"});
        expectSnapshot("MIXED", "enabled", {SAI_OBJECT_TYPE_PORT, SAI_OBJECT_TYPE_QUEUE});
        EXPECT_NE(row("MIXED").at("object_ids"), original.at("object_ids"));
    }

    TEST_F(HFTelSessionOwnerTest, MixedReadyReconfigurationReplacesSnapshotWithoutRemovingOwner)
    {
        createOrch();
        twoGroups();
        const auto original = row("MIXED");
        setGroup("PORT", "Ethernet8");
        EXPECT_EQ(row("MIXED"), original);
        expectKeys({"MIXED"});
        EXPECT_EQ(profile->getStreamState(SAI_OBJECT_TYPE_QUEUE), SAI_TAM_TEL_TYPE_STATE_CREATE_CONFIG);
        EXPECT_EQ(orch->profileTableSet(name, {{"stream_state", "disabled"}}), task_need_retry);
        ready(SAI_OBJECT_TYPE_PORT, 600);
        expectSnapshot("MIXED", "enabled", {SAI_OBJECT_TYPE_PORT, SAI_OBJECT_TYPE_QUEUE});
        expectKeys({"MIXED"});
        EXPECT_NE(row("MIXED").at("session_config"), original.at("session_config"));
    }

    TEST_F(HFTelSessionOwnerTest, MixedDisabledGroupDeletionPublishesDisabledReplacement)
    {
        createOrch();
        twoGroups();
        ASSERT_EQ(orch->profileTableSet(name, {{"stream_state", "disabled"}}), task_success);
        ASSERT_EQ(orch->groupTableDel(name, "QUEUE"), task_success);
        ready(SAI_OBJECT_TYPE_PORT, 500);
        expectSnapshot("MIXED", "disabled", {SAI_OBJECT_TYPE_PORT});
        ASSERT_EQ(orch->profileTableSet(name, {{"stream_state", "enabled"}}), task_success);
        expectSnapshot("MIXED", "enabled", {SAI_OBJECT_TYPE_PORT});
        expectKeys({"MIXED"});
    }

    TEST_F(HFTelSessionOwnerTest, SingleKeepsIndependentGroupRowsAndLifecycle)
    {
        createOrch(false);
        setGroup("PORT", "Ethernet0,Ethernet4");
        ready(SAI_OBJECT_TYPE_PORT);
        expectSnapshot("PORT", "enabled", {SAI_OBJECT_TYPE_PORT});
        const auto port = row("PORT");
        setGroup("QUEUE", "Ethernet0|0,Ethernet4|0");
        ready(SAI_OBJECT_TYPE_QUEUE, 400);
        expectSnapshot("QUEUE", "enabled", {SAI_OBJECT_TYPE_QUEUE});
        expectKeys({"PORT", "QUEUE"});
        EXPECT_EQ(row("PORT"), port);
        ASSERT_EQ(orch->profileTableSet(name, {{"stream_state", "disabled"}}), task_success);
        EXPECT_EQ(row("PORT").at("stream_status"), "disabled");
        EXPECT_EQ(row("QUEUE").at("stream_status"), "disabled");
        ASSERT_EQ(orch->profileTableSet(name, {{"stream_state", "enabled"}}), task_success);
        EXPECT_EQ(row("PORT"), port);
        const auto queue = row("QUEUE");
        ASSERT_EQ(orch->groupTableDel(name, "PORT"), task_success);
        EXPECT_EQ(row("QUEUE"), queue);
        EXPECT_EQ(profile->getStreamState(SAI_OBJECT_TYPE_QUEUE), SAI_TAM_TEL_TYPE_STATE_START_STREAM);
        expectKeys({"QUEUE"});
        ASSERT_EQ(orch->groupTableDel(name, "QUEUE"), task_success);
        expectKeys({});
    }

    /*
     * SAI_TAM_TEL_TYPE_ATTR_MODE capability query returns NOT_SUPPORTED
     * (current saivs behavior). HFT should still be enabled via the
     * spec-default SINGLE_TYPE fallback.
     */
    TEST_F(HFTelOrchModeTest, IsSupportedHFTel_mode_query_not_supported)
    {
        HFTelSaiHookGuard guard(hftelorch_sai_wrap_ut::setSaiHookModeQueryNotSupported);
        EXPECT_TRUE(HFTelOrch::isSupportedHFTel(gSwitchId));
    }
}
