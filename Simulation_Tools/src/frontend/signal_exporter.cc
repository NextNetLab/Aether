/* -*-mode:c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include "signal_exporter.hh"
#include "bpf_signal_exporter.hh"

namespace cellular_emulation {

std::shared_ptr<SignalExporter>
SignalExporterFactory::create(const std::string& type_str,
                              const std::string& config)
{
    if (type_str == "file") {
        return std::make_shared<FileSignalExporter>(config);
    }
    if (type_str == "csv") {
        return std::make_shared<CSVSignalLogger>(config);
    }
    if (type_str == "bpf_queue") {
        return std::make_shared<BpfQueueExporter>();
    }
    if (type_str == "multi") {
        auto multi = std::make_shared<MultiExporter>();
        /* Try BPF first. If it fails to attach, its export_signal()
         * returns false and is_ready() reports false, which the
         * caller can inspect. We still add it so the CSV log gives
         * a ground-truth comparison. */
        multi->add_exporter(std::make_shared<BpfQueueExporter>());
        multi->add_exporter(std::make_shared<CSVSignalLogger>(
            config.empty() ? "/tmp/mm_vdqueue_log.csv" : config));
        return multi;
    }
    return std::make_shared<FileSignalExporter>(config);
}

} // namespace cellular_emulation
