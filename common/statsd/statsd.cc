#include "common/statsd/statsd.h"
#include "common/Counters_impl.h"

extern "C" {
#include "statsd-client.h"
}

#include "absl/strings/str_cat.h"
#include "absl/strings/str_replace.h"

#include <string>

using namespace std;

namespace sorbet {

class StatsdClientWrapper {
    constexpr static int PKT_LEN = 512; // conservative bound for MTU
    statsd_link *link;
    string packet;

    string cleanMetricName(const string &name) {
        return absl::StrReplaceAll(name, {{":", "_"}, {"|", "_"}, {"@", "_"}});
    }

    void addMetric(const string &name, size_t value, const string &type) {
        // spec: https://github.com/etsy/statsd/blob/master/docs/metric_types.md#multi-metric-packets
        auto newLine = fmt::format("{}{}:{}|{}", link->ns ? link->ns : "", cleanMetricName(name), value, type);
        if (packet.size() + newLine.size() + 1 < PKT_LEN) {
            packet = packet + "\n" + newLine;
        } else {
            if (packet.size() > 0) {
                statsd_send(link, packet.c_str());
                packet = move(newLine);
            } else {
                // the packet itself might be bigger than MTU
                statsd_send(link, newLine.c_str());
            }
        }
    }

public:
    StatsdClientWrapper(string host, int port, string prefix)
        : link(statsd_init_with_namespace(host.c_str(), port, cleanMetricName(prefix).c_str())) {}

    ~StatsdClientWrapper() {
        if (packet.size() > 0) {
            statsd_send(link, packet.c_str());
        }
        statsd_finalize(link);
    }

    void gauge(const string &name, size_t value) { // type : g
        addMetric(name, value, "g");
    }
    void timing(const string &name, size_t ns) {    // type: ms
        addMetric(name + ".duration_ns", ns, "ms"); // format suggested by #observability (@sjung and @an)
    }
};

bool StatsD::submitCounters(const CounterState &counters, string host, int port, string prefix) {
    StatsdClientWrapper statsd(host, port, prefix);

    counters.counters->canonicalize();
    for (auto &cat : counters.counters->counters_by_category) {
        CounterImpl::CounterType sum = 0;
        for (auto &e : cat.second) {
            sum += e.second;
            statsd.gauge(absl::StrCat(cat.first, ".", e.first), e.second);
        }

        statsd.gauge(absl::StrCat(cat.first, ".total"), sum);
    }

    for (auto &hist : counters.counters->histograms) {
        CounterImpl::CounterType sum = 0;
        for (auto &e : hist.second) {
            sum += e.second;
            statsd.gauge(absl::StrCat(hist.first, ".", e.first), e.second);
        }

        statsd.gauge(absl::StrCat(hist.first, ".total"), sum);
    }

    for (auto &e : counters.counters->counters) {
        statsd.gauge(e.first, e.second);
    }

    for (auto &e : counters.counters->timings) {
        for (auto entry : e.second) {
            statsd.timing(e.first, entry);
        }
    }

    return true;
}

} // namespace sorbet
