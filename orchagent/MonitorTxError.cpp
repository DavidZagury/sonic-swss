//
// Created by davidza on 1/4/21.
//
#include <sstream>
#include <inttypes.h>
#include <string>

#include "converter.h"
#include "MonitorTxError.h"
#include "timer.h"
#include "port.h"
#include "select.h"
#include "portsorch.h"
#include "sai_serialize.h"
#include <array>

using namespace std;
using namespace swss;

#define TX_POOLING_PERIOD "pooling_period"
#define TX_THRESHOLD "threshold"

#define POOLING_PERIOD_KEY  "POOLING_PERIOD"
#define THRESHOLD_KEY "THRESHOLD"

#define STATE_DB_TX_STATE "port_state"
#define VALUE "value"
#define STATES_NUMBER 3

static const array<string, STATES_NUMBER> stateNames = { "OK", "NOT_OK", "UNKNOWN" };
static const string counterName = "SAI_PORT_STAT_IF_OUT_ERRORS";
static const string portNameMap = "COUNTERS_PORT_NAME_MAP";

MonitorTxError::MonitorTxError(TableConnector configDBTConnector, TableConnector stateDBTConnector) :
        Orch(configDBTConnector.first, configDBTConnector.second),
        m_countersDB(new DBConnector(COUNTERS_DB, DBConnector::DEFAULT_UNIXSOCKET, 0)),
        m_countersTable(new Table(m_countersDB.get(), "COUNTERS")),
        m_countersPortNameMap(new Table(m_countersDB.get(), COUNTERS_PORT_NAME_MAP)),
        m_stateTxErrTable(stateDBTConnector.first, stateDBTConnector.second),
        m_txErrConfigTable(configDBTConnector.first, configDBTConnector.second)
{
    SWSS_LOG_ENTER();
    initTimer(TX_ERROR_CHECK_DEFAULT_TIMER);

    vector <FieldValueTuple> fv;
    fv.emplace_back(VALUE, to_string(m_poolingPeriod_packets));
    m_txErrConfigTable.set(POOLING_PERIOD_KEY, fv);
    fv.clear();
    fv.emplace_back(VALUE, to_string(m_threshold_sec));
    m_txErrConfigTable.set(THRESHOLD_KEY, fv);

    SWSS_LOG_DEBUG("MonitorTxError initialized\n");
}

void MonitorTxError::doTask(Consumer& consumer)
{
    SWSS_LOG_ENTER();
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string key = kfvKey(t);
        string op = kfvOp(t);
        vector <FieldValueTuple> fvs = kfvFieldsValues(t);

        if (op == SET_COMMAND)
        {
            if (key == "polling_period")
            {
                setThreshold(fvs);
            }
            else if (key == "threshold")
            {
                setPoolingPeriod(fvs);
            }
            else
            {
                SWSS_LOG_ERROR("Unexpected key %s", key.c_str());
            }
        }
        else
        {
            SWSS_LOG_ERROR("Unexpected operation %s", op.c_str());
        }
        consumer.m_toSync.erase(it++);
    }
}

void MonitorTxError::doTask(SelectableTimer& timer)
{
    SWSS_LOG_ENTER();
    if (!gPortsOrch->allPortsReady())
    {
        return;
    }
    updateAllPortsState();
}

void MonitorTxError::updateAllPortsState()
{
    SWSS_LOG_ENTER();
    map <string, Port>& portsList = gPortsOrch->getAllPorts();
    for (auto& entry : portsList)
    {
        string name = entry.first;
        Port p = entry.second;

        if (p.m_type != Port::PHY)
        {
            continue;
        }

        string oidStr;
        if (!m_countersPortNameMap->hget("", name, oidStr))
        {
            SWSS_LOG_ERROR("error getting port name from counters");
            continue;
        }
        updatePortState(oidStr, p);
    }
}

void MonitorTxError::updatePortState(string oidStr, Port& port)
{
    PortState portState = OK;
    string txErrStrValue;
    uint64_t txErrValue;
    uint64_t txErrDiff;

    if (!m_countersTable->hget(oidStr, counterName, txErrStrValue))
    {
        portState = UNKNOWN;
        SWSS_LOG_ERROR("Cannot take information from table for port %s", oidStr.c_str());
        return;
    }
    try
    {
        txErrValue = stoul(txErrStrValue);
    }
    catch (...)
    {
        SWSS_LOG_ERROR("Cannot change status.");
        return;
    }

    txErrDiff = txErrValue - m_lastTxCounters[port.m_alias];
    if (txErrDiff > m_threshold_sec)
    {
        portState = NOT_OK;
    }

    vector <FieldValueTuple> fieldValuesVector;
    fieldValuesVector.emplace_back(STATE_DB_TX_STATE, stateNames[portState]);
    m_stateTxErrTable.set(port.m_alias, fieldValuesVector);

    /* save data for the next update */
    m_lastTxCounters[port.m_alias] = txErrValue;
}

MonitorTxError::~MonitorTxError(void)
{
    SWSS_LOG_ENTER();
}

void MonitorTxError::initTimer(int setPoolingPeriod)
{
    SWSS_LOG_ENTER();
    m_poolingPeriod_packets = setPoolingPeriod;
    auto interv = timespec{ .tv_sec = m_poolingPeriod_packets, .tv_nsec = 0 };
    timer = new SelectableTimer(interv);
    auto executor = new ExecutableTimer(timer, this, "TX_ERROR_POOLING_PERIOD");
    Orch::addExecutor(executor);
    timer->start();
}

void MonitorTxError::setThreshold(const vector <FieldValueTuple>& data)
{
    SWSS_LOG_ENTER();
    for (auto element : data)
    {
        const auto& field = fvField(element);
        const auto& value = fvValue(element);
        if (field == VALUE)
        {
            try
            {
                m_threshold_sec = stoi(value);
                SWSS_LOG_NOTICE("Changing threshold value to %s", value.c_str());
            }
            catch (...)
            {
                SWSS_LOG_WARN("Cannot change threshold to the value entered.");
            }
        }
        else
        {
            SWSS_LOG_WARN("Unknown field value");
        }
    }
}

void MonitorTxError::setPoolingPeriod(const vector <FieldValueTuple>& data)
{
    SWSS_LOG_ENTER();
    for (auto element : data)
    {
        const auto& field = fvField(element);
        const auto& value = fvValue(element);
        if (field == VALUE)
        {
            try
            {
                // change the interval and reset timer
                m_poolingPeriod_packets = stoi(value);
                auto interv = timespec{ .tv_sec = m_poolingPeriod_packets, .tv_nsec = 0 };
                timer->setInterval(interv);
                timer->reset();
                SWSS_LOG_NOTICE("Changing pooling_period value to %s", value.c_str());
            }
            catch (...)
            {
                SWSS_LOG_WARN("Cannot change pooling_period to the value entered.");
            }
        }
        else
        {
            SWSS_LOG_WARN("Unknown field value");
        }
    }
}
