/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*
 * Copyright (C) 2016 ScyllaDB
 */

#include "prometheus.hh"
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include "proto/metrics2.pb.h"

#include "scollectd_api.hh"
#include "scollectd-impl.hh"
#include "http/function_handlers.hh"
#include <boost/algorithm/string/replace.hpp>
#include <boost/range/algorithm_ext/erase.hpp>
#include <boost/algorithm/string.hpp>

namespace prometheus {
namespace pm = io::prometheus::client;

/**
 * Taken from an answer in stackoverflow:
 * http://stackoverflow.com/questions/2340730/are-there-c-equivalents-for-the-protocol-buffers-delimited-i-o-functions-in-ja
 */
static bool write_delimited_to(const google::protobuf::MessageLite& message,
        google::protobuf::io::ZeroCopyOutputStream* rawOutput) {
    google::protobuf::io::CodedOutputStream output(rawOutput);

    const int size = message.ByteSize();
    output.WriteVarint32(size);

    uint8_t* buffer = output.GetDirectBufferForNBytesAndAdvance(size);
    if (buffer != nullptr) {
        message.SerializeWithCachedSizesToArray(buffer);
    } else {
        message.SerializeWithCachedSizes(&output);
        if (output.HadError()) {
            return false;
        }
    }

    return true;
}

static std::string safe_name(const sstring& name) {
    auto rep = boost::replace_all_copy(boost::replace_all_copy(name, "-", "_"), " ", "_");
    boost::remove_erase_if(rep, boost::is_any_of("+()"));
    return rep;
}

static sstring collectd_name(const scollectd::type_instance_id & id, uint32_t cpu) {
    return safe_name(id.plugin()) + "_" + safe_name(id.type_instance());
}

static pm::Metric* add_label(pm::Metric* mt, const scollectd::type_instance_id & id, uint32_t cpu) {
    auto label = mt->add_label();
    label->set_name("shard");
    label->set_value(std::to_string(cpu));
    label = mt->add_label();
    label->set_name("type");
    label->set_value(id.type());

    if (id.type_instance() != "") {
        label = mt->add_label();
        label->set_name("metric");
        label->set_value(id.type_instance());
    }
    const sstring& host = scollectd::get_impl().host();
    if (host != "") {
        label = mt->add_label();
        label->set_name("instance");
        label->set_value(host);
    }
    return mt;
}

static void fill_metric(pm::MetricFamily& mf, const std::vector<scollectd::collectd_value>& vals,
        const scollectd::type_instance_id & id, uint32_t cpu) {
    for (const scollectd::collectd_value& c : vals) {
        switch (c.type()) {
        case scollectd::data_type::DERIVE:
            add_label(mf.add_metric(), id, cpu)->mutable_counter()->set_value(c.i());
            mf.set_type(pm::MetricType::COUNTER);
            break;
        case scollectd::data_type::GAUGE:
            add_label(mf.add_metric(), id, cpu)->mutable_gauge()->set_value(c.d());
            mf.set_type(pm::MetricType::GAUGE);
            break;
        default:
            add_label(mf.add_metric(), id, cpu)->mutable_counter()->set_value(c.ui());
            mf.set_type(pm::MetricType::COUNTER);
            break;
        }
    }

}

future<> start(httpd::http_server_control& http_server, const config& ctx) {
    return http_server.set_routes([&ctx](httpd::routes& r) {
        httpd::future_handler_function f = [&ctx](std::unique_ptr<request> req, std::unique_ptr<reply> rep) {
            return do_with(std::vector<scollectd::value_map>(), [rep = std::move(rep), &ctx] (auto& vec) mutable {
                vec.resize(smp::count);
                return parallel_for_each(boost::irange(0u, smp::count), [&vec] (auto cpu) {
                    return smp::submit_to(cpu, [] {
                        return scollectd::get_value_map();
                    }).then([&vec, cpu] (auto res) {
                        vec[cpu] = res;
                    });
                }).then([rep = std::move(rep), &vec, &ctx]() mutable {
                    std::unordered_map<sstring, std::vector<std::pair<unsigned, scollectd::value_map::value_type*>>> families;
                    uint32_t cpu = 0;
                    for (auto&& shard : vec) {
                        for (auto&& metric : shard) {
                            auto name = ctx.prefix + "_" + collectd_name(metric.first, cpu);
                            families[name].push_back(std::make_pair(cpu, &metric));
                        }
                        cpu++;
                    }
                    std::string s;
                    google::protobuf::io::StringOutputStream os(&s);
                    for (auto name_metrics : families) {
                        auto&& name = name_metrics.first;
                        auto&& metrics = name_metrics.second;
                        pm::MetricFamily mtf;
                        mtf.set_name(name);
                        mtf.set_help("where did the description disappear?");
                        for (auto p_cpu_metric : metrics) {
                            auto cpu = p_cpu_metric.first;
                            auto&& id = p_cpu_metric.second->first;
                            auto&& value = p_cpu_metric.second->second;
                            fill_metric(mtf, value, id, cpu);
                        }
                        if (!write_delimited_to(mtf, &os)) {
                            seastar_logger.warn("Failed to write protobuf metrics");
                        }
                    }
                    rep->_content = s;
                    return make_ready_future<std::unique_ptr<reply>>(std::move(rep));
                });
            });
        };
        r.put(GET, "/metrics", new httpd::function_handler(f, "proto"));
    });
}

}
