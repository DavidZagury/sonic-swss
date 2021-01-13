//
// Created by davidza on 1/4/21.
//

#ifndef SONIC_SWSS_MONITORTXERROR_H
#define SONIC_SWSS_MONITORTXERROR_H

#include <map>
#include <string>

#include "orch.h"
#include "portsorch.h"
#include "table.h"
#include "selectabletimer.h"
#include "select.h"
#include "timer.h"

using namespace std;
using namespace swss;
const time_t TX_ERROR_CHECK_DEFAULT_TIMER = 10;
const uint64_t TX_ERROR_DEFAULT_THRESHOLD = 100;
extern PortsOrch* gPortsOrch;
enum PortState
{
	OK, NOT_OK, UNKNOWN
};

class MonitorTxError : public Orch
{
public:
	MonitorTxError(TableConnector configDBTConnector, TableConnector stateDBTConnector);
	virtual void doTask(swss::SelectableTimer& timer);
	virtual void doTask(Consumer& consumer);
	virtual ~MonitorTxError(void);

private:
	SelectableTimer* timer = nullptr;
	// counters table - taken from redis
	shared_ptr <swss::DBConnector> m_countersDB = nullptr;
	shared_ptr <swss::Table> m_countersTable = nullptr;
	shared_ptr <Table> m_countersPortNameMap = nullptr;

	// state table
	Table m_stateTxErrTable;
	Table m_txErrConfigTable;

	// counetrs of tx errors
	map <string, uint64_t> m_portsStateTable;
	map <string, uint64_t> m_lastTxCounters;

	uint64_t m_threshold_sec = TX_ERROR_DEFAULT_THRESHOLD;
	int m_poolingPeriod_packets = TX_ERROR_CHECK_DEFAULT_TIMER;

	void initTimer(int setPoolingPeriod);
	void updateAllPortsState();
    void updatePortState(string portAlias, Port& port);
	void setThreshold(const vector <FieldValueTuple>& data);
	void setPoolingPeriod(const vector <FieldValueTuple>& data);
	void setDefaultConfigParam();
};

#endif //SONIC_SWSS_MONITORTXERROR_H
