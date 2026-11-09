// SPDX-License-Identifier: MIT
// Rust: src/api/impls/template_helpers.rs
#include "agentenv/api/template_helpers.h"

#include "agentenv/cfg.h"
// `TemplateModel` reports its timestamps as RFC3339, as upstream's
// `DateTime<Utc>` does.
#include "agentenv/core/time.h"

namespace agentenv {
namespace api {

// ---------------------------------------------------------------------------
// Request / response DTOs
// ---------------------------------------------------------------------------

namespace {

/// A `u32` schema field. Present-but-wrong-type is an error rather than a
/// silent default, matching `ParseResourceField` in `dto.cc` and the generated
/// deserializer it stands in for: a negative value read as a huge unsigned
/// count would pass `ResolveTemplateResources`' only check (non-zero) and size
/// a build nobody asked for.
core::Expected<core::Optional<uint32_t>, core::AnyError> ParseU32Field(
    const core::Json& j, const std::string& key) {
    const core::Json& v = j[key];
    if (v.kind() == core::Json::Kind::Null) return core::Optional<uint32_t>();
    if (v.kind() != core::Json::Kind::Int || v.as_int() < 0 || v.as_int() > 0xFFFFFFFFll) {
        return core::make_unexpected(
            core::err("invalid field: " + key + " must be a non-negative integer"));
    }
    return core::Optional<uint32_t>(static_cast<uint32_t>(v.as_int()));
}

}  // namespace

namespace {

/// An optional string field. Present-but-wrong-type is an error, not an
/// absent value: these select the base image, the start command and the
/// ready check, so reading `123` as "omitted" would build something other
/// than what was asked for and report nothing.
core::Expected<core::Optional<std::string>, core::AnyError> ParseOptionalString(
    const core::Json& j, const std::string& key) {
    const core::Json& v = j[key];
    if (v.kind() == core::Json::Kind::Null) return core::Optional<std::string>();
    if (v.kind() != core::Json::Kind::String) {
        return core::make_unexpected(core::err("invalid field: " + key + " must be a string"));
    }
    return core::Optional<std::string>(v.as_string());
}

core::Expected<core::Optional<bool>, core::AnyError> ParseOptionalBool(const core::Json& j,
                                                                      const std::string& key) {
    const core::Json& v = j[key];
    if (v.kind() == core::Json::Kind::Null) return core::Optional<bool>();
    if (v.kind() != core::Json::Kind::Bool) {
        return core::make_unexpected(core::err("invalid field: " + key + " must be a boolean"));
    }
    return core::Optional<bool>(v.as_bool());
}

}  // namespace

core::Expected<TemplateStepRequest, core::AnyError> TemplateStepRequest::FromJson(
    const core::Json& j) {
    if (j.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(core::err("invalid field: steps must contain objects"));
    }
    TemplateStepRequest step;

    // The schema's only required field, and the one that selects the step
    // kind. Absent is an error rather than an empty string, which
    // `ApplyE2bTemplateStep` would report as an unknown step type -- blaming
    // a value the caller never sent.
    const core::Json& type = j["type"];
    if (type.kind() != core::Json::Kind::String) {
        return core::make_unexpected(core::err("invalid field: steps[].type must be a string"));
    }
    step.type = type.as_string();

    const core::Json& args = j["args"];
    if (args.kind() == core::Json::Kind::Array) {
        std::vector<std::string> parsed;
        const core::JsonArray& items = args.as_array();
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (items[i].kind() != core::Json::Kind::String) {
                return core::make_unexpected(
                    core::err("invalid field: steps[].args must be strings"));
            }
            parsed.push_back(items[i].as_string());
        }
        // An explicit `[]` stays an empty list: `ApplyE2bTemplateStep` reads
        // the arity to decide whether a step is well-formed, so "no args" and
        // "args omitted" must not collapse into one.
        step.args = core::Optional<std::vector<std::string> >(parsed);
    } else if (args.kind() != core::Json::Kind::Null) {
        return core::make_unexpected(core::err("invalid field: steps[].args must be an array"));
    }

    const core::Expected<core::Optional<std::string>, core::AnyError> files_hash =
        ParseOptionalString(j, "filesHash");
    if (!files_hash.ok()) return core::make_unexpected(files_hash.error());
    step.files_hash = files_hash.value();

    const core::Expected<core::Optional<bool>, core::AnyError> force =
        ParseOptionalBool(j, "force");
    if (!force.ok()) return core::make_unexpected(force.error());
    step.force = force.value();
    return step;
}

core::Expected<TemplateBuildStartRequest, core::AnyError> TemplateBuildStartRequest::FromJson(
    const core::Json& j) {
    TemplateBuildStartRequest body;

    const char* const string_keys[] = {"fromImage", "fromTemplate", "startCmd", "readyCmd"};
    core::Optional<std::string>* const string_fields[] = {&body.from_image, &body.from_template,
                                                          &body.start_cmd, &body.ready_cmd};
    for (std::size_t i = 0; i < sizeof(string_keys) / sizeof(string_keys[0]); ++i) {
        const core::Expected<core::Optional<std::string>, core::AnyError> value =
            ParseOptionalString(j, string_keys[i]);
        if (!value.ok()) return core::make_unexpected(value.error());
        *string_fields[i] = value.value();
    }

    const core::Expected<core::Optional<bool>, core::AnyError> force =
        ParseOptionalBool(j, "force");
    if (!force.ok()) return core::make_unexpected(force.error());
    body.force = force.value();

    // Kept as raw JSON; the handler refuses it rather than ignoring it. A
    // non-object is still rejected here, because a `fromImageRegistry` that
    // cannot be a credential block is a malformed body rather than an
    // unsupported feature.
    const core::Json& registry = j["fromImageRegistry"];
    if (registry.kind() == core::Json::Kind::Object) {
        body.from_image_registry = core::Optional<core::Json>(registry);
    } else if (registry.kind() != core::Json::Kind::Null) {
        return core::make_unexpected(
            core::err("invalid field: fromImageRegistry must be an object"));
    }

    const core::Json& steps = j["steps"];
    if (steps.kind() == core::Json::Kind::Array) {
        std::vector<TemplateStepRequest> parsed;
        const core::JsonArray& items = steps.as_array();
        for (std::size_t i = 0; i < items.size(); ++i) {
            const core::Expected<TemplateStepRequest, core::AnyError> step =
                TemplateStepRequest::FromJson(items[i]);
            if (!step.ok()) return core::make_unexpected(step.error());
            parsed.push_back(step.value());
        }
        body.steps = core::Optional<std::vector<TemplateStepRequest> >(parsed);
    } else if (steps.kind() != core::Json::Kind::Null) {
        return core::make_unexpected(core::err("invalid field: steps must be an array"));
    }
    return body;
}

core::Expected<TemplateBuildRequestV3, core::AnyError> TemplateBuildRequestV3::FromJson(
    const core::Json& j) {
    TemplateBuildRequestV3 body;

    const core::Json& name = j["name"];
    if (name.kind() == core::Json::Kind::String) {
        body.name = core::Optional<std::string>(name.as_string());
    } else if (name.kind() != core::Json::Kind::Null) {
        return core::make_unexpected(core::err("invalid field: name must be a string"));
    }

    const core::Expected<core::Optional<uint32_t>, core::AnyError> cpu =
        ParseU32Field(j, "cpuCount");
    if (!cpu.ok()) return core::make_unexpected(cpu.error());
    body.cpu_count = cpu.value();

    const core::Expected<core::Optional<uint32_t>, core::AnyError> memory =
        ParseU32Field(j, "memoryMB");
    if (!memory.ok()) return core::make_unexpected(memory.error());
    body.memory_mb = memory.value();

    const core::Json& tags = j["tags"];
    if (tags.kind() == core::Json::Kind::Array) {
        std::vector<std::string> parsed;
        const core::JsonArray& items = tags.as_array();
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (items[i].kind() != core::Json::Kind::String) {
                return core::make_unexpected(core::err("invalid field: tags must be strings"));
            }
            parsed.push_back(items[i].as_string());
        }
        // An explicit `[]` is kept as an empty list rather than folded into
        // "absent": `TemplateBuildRecordFromV3Request` refuses a *non-empty*
        // tag list, so the two are already equivalent there -- but collapsing
        // them here would make the DTO lossy for no reason.
        body.tags = core::Optional<std::vector<std::string> >(parsed);
    } else if (tags.kind() != core::Json::Kind::Null) {
        return core::make_unexpected(core::err("invalid field: tags must be an array"));
    }

    const core::Json& image_only = j["imageOnly"];
    if (image_only.kind() == core::Json::Kind::Bool) {
        body.image_only = core::Optional<bool>(image_only.as_bool());
    } else if (image_only.kind() != core::Json::Kind::Null) {
        // Not defaulted to false on a wrong type: `imageOnly` selects between
        // two different kinds of build, so guessing it is the one field here
        // that can satisfy a request with the wrong artifact.
        return core::make_unexpected(core::err("invalid field: imageOnly must be a boolean"));
    }
    return body;
}

core::Json TemplateBuildRequestV3::ToJson() const {
    core::Json j;
    if (name.has_value()) j["name"] = core::Json(*name);
    if (cpu_count.has_value()) j["cpuCount"] = core::Json(static_cast<int64_t>(*cpu_count));
    if (memory_mb.has_value()) j["memoryMB"] = core::Json(static_cast<int64_t>(*memory_mb));
    if (tags.has_value()) {
        core::JsonArray items;
        for (std::size_t i = 0; i < tags->size(); ++i) {
            items.emplace_back(core::Json((*tags)[i]));
        }
        j["tags"] = core::Json(items);
    }
    if (image_only.has_value()) j["imageOnly"] = core::Json(*image_only);
    return j;
}

namespace {

core::JsonArray StringsToJson(const std::vector<std::string>& values) {
    core::JsonArray items;
    for (std::size_t i = 0; i < values.size(); ++i) {
        items.emplace_back(core::Json(values[i]));
    }
    return items;
}

std::vector<std::string> StringsFromJson(const core::Json& j) {
    std::vector<std::string> out;
    if (j.kind() != core::Json::Kind::Array) return out;
    const core::JsonArray& items = j.as_array();
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (items[i].kind() == core::Json::Kind::String) out.push_back(items[i].as_string());
    }
    return out;
}

}  // namespace

core::Json TemplateRequestResponseV3::ToJson() const {
    core::Json j;
    j["templateID"] = core::Json(template_id);
    j["buildID"]    = core::Json(build_id);
    j["public"]     = core::Json(is_public);
    // All three are required by the schema, so they are written even when
    // empty: a client reading `names[0]` must not have to handle the key
    // being absent.
    j["names"]   = core::Json(StringsToJson(names));
    j["tags"]    = core::Json(StringsToJson(tags));
    j["aliases"] = core::Json(StringsToJson(aliases));
    return j;
}

core::Expected<TemplateRequestResponseV3, core::AnyError> TemplateRequestResponseV3::FromJson(
    const core::Json& j) {
    TemplateRequestResponseV3 r;
    r.template_id = j["templateID"].kind() == core::Json::Kind::String
                        ? j["templateID"].as_string()
                        : std::string();
    r.build_id    = j["buildID"].kind() == core::Json::Kind::String ? j["buildID"].as_string()
                                                                   : std::string();
    r.is_public   = j["public"].kind() == core::Json::Kind::Bool ? j["public"].as_bool() : false;
    r.names       = StringsFromJson(j["names"]);
    r.tags        = StringsFromJson(j["tags"]);
    r.aliases     = StringsFromJson(j["aliases"]);
    return r;
}

// ---------------------------------------------------------------------------
// TemplateModel
// ---------------------------------------------------------------------------

snapshot::TemplateBuildStatus TemplateBuildStatusOf(const snapshot::SnapshotRecord& record) {
    // A sandbox-sourced record has no build to report on, and it only exists
    // because a capture already succeeded -- so `Ready`, not the `Waiting`
    // that a default-constructed `TemplateBuildInfo` would give.
    if (!record.source.is_template()) return snapshot::TemplateBuildStatus::Ready;
    return record.source.build.status;
}

const char* TemplateBuildStatusWireName(snapshot::TemplateBuildStatus status) {
    switch (status) {
        case snapshot::TemplateBuildStatus::Waiting:
            return "waiting";
        case snapshot::TemplateBuildStatus::Building:
            return "building";
        case snapshot::TemplateBuildStatus::Ready:
            return "ready";
        case snapshot::TemplateBuildStatus::Error:
        default:
            return "error";
    }
}

TemplateModel TemplateModel::FromRecord(const snapshot::SnapshotRecord& record) {
    TemplateModel model;
    model.template_id  = record.id.ToString();
    model.build_id     = record.id.ToString();
    model.cpu_count    = record.resources.cpu_count;
    model.memory_mb    = record.resources.memory_mib;
    model.disk_size_mb = record.resources.disk_size_mib;
    model.is_public    = true;

    // Rust `build_record_names`: the alias when it has one, otherwise the id.
    // Never empty, because `names` is how a client addresses the template and
    // an unaliased one is still addressable by id.
    if (record.alias.has_value()) {
        model.names.push_back(record.alias->ToString());
    } else {
        model.names.push_back(record.id.ToString());
    }

    model.created_at_unix_ms = record.created_at_unix_ms;
    model.updated_at_unix_ms = record.updated_at_unix_ms;
    model.spawn_count        = 0;
    model.build_count        = 1;

    // Rust `build_record_envd_version`. A waiting or building record has no
    // committed snapshot, so there is no version yet -- reported as
    // `"unknown"` rather than as an empty string, which a client would have
    // to special-case separately from a genuinely missing field.
    model.envd_version = record.committed.has_value()
                             ? record.committed->runtime_versions.envd_version
                             : std::string("unknown");
    model.build_status = TemplateBuildStatusOf(record);
    return model;
}

core::Json TemplateModel::ToJson() const {
    core::Json j;
    j["templateID"]  = core::Json(template_id);
    j["buildID"]     = core::Json(build_id);
    j["cpuCount"]    = core::Json(static_cast<int64_t>(cpu_count));
    j["memoryMB"]    = core::Json(static_cast<int64_t>(memory_mb));
    j["diskSizeMB"]  = core::Json(static_cast<int64_t>(disk_size_mb));
    j["public"]      = core::Json(is_public);
    j["names"]       = core::Json(StringsToJson(names));
    // RFC3339, as upstream's `DateTime<Utc>` serialises, and as
    // `SnapshotInfoResp` already does on the snapshot route.
    j["createdAt"]   = core::Json(
        core::FormatRfc3339Nanos(core::SystemTime(created_at_unix_ms * 1000000)));
    j["updatedAt"]   = core::Json(
        core::FormatRfc3339Nanos(core::SystemTime(updated_at_unix_ms * 1000000)));
    // Written as `null`, not omitted: the schema declares the key as nullable
    // and required, so a client reading it must find it present.
    j["lastSpawnedAt"] =
        last_spawned_at_unix_ms.has_value()
            ? core::Json(core::FormatRfc3339Nanos(
                  core::SystemTime(*last_spawned_at_unix_ms * 1000000)))
            : core::Json();
    j["spawnCount"]  = core::Json(spawn_count);
    j["buildCount"]  = core::Json(static_cast<int64_t>(build_count));
    j["envdVersion"] = core::Json(envd_version);
    j["buildStatus"] = core::Json(TemplateBuildStatusWireName(build_status));
    return j;
}

// ---------------------------------------------------------------------------
// The build-status body and its log query
// ---------------------------------------------------------------------------

namespace {

/// A query parameter that must be a non-negative integer, or absent.
///
/// Reported rather than defaulted on garbage, which is what the generated
/// layer upstream does: `limit=abc` silently reading as "unset" would answer
/// 200 with a page size the caller never asked for, and they have no way to
/// tell that from the server honouring the request.
core::Expected<core::Optional<int64_t>, core::AnyError> ParseQueryInt(
    const std::string& text, const char* name) {
    if (text.empty()) return core::Optional<int64_t>();
    // Accumulated *unsigned*, which is both what Rust does (`cursor` is a
    // `u64` in the schema, narrowed afterwards by `i64::try_from`) and what
    // makes the bound below a defined comparison rather than signed overflow.
    uint64_t value = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c < '0' || c > '9') {
            return core::make_unexpected(
                core::err("invalid query parameter: " + std::string(name) +
                          " must be a non-negative integer"));
        }
        const uint64_t digit = static_cast<uint64_t>(c - '0');
        // Rust's `i64::try_from` on the parsed `u64`, with its own 400: a
        // value past `i64::MAX` cannot be compared against a timestamp at
        // all, and letting it through would wrap into a negative one -- a
        // bound that filters nothing, reported as success.
        if (value > (0x7FFFFFFFFFFFFFFFull - digit) / 10) {
            return core::make_unexpected(core::err(
                "invalid query parameter: " + std::string(name) + " exceeds int64 range"));
        }
        value = value * 10 + digit;
    }
    return core::Optional<int64_t>(static_cast<int64_t>(value));
}

}  // namespace

core::Expected<BuildLogQuery, core::AnyError> BuildLogQuery::ForStatus(
    const std::string& logs_offset, const std::string& limit, const std::string& level) {
    BuildLogQuery query;

    const core::Expected<core::Optional<int64_t>, core::AnyError> offset =
        ParseQueryInt(logs_offset, "logsOffset");
    if (!offset.ok()) return core::make_unexpected(offset.error());
    if (offset.value().has_value()) {
        query.offset = static_cast<std::size_t>(*offset.value());
    }

    const core::Expected<core::Optional<int64_t>, core::AnyError> parsed_limit =
        ParseQueryInt(limit, "limit");
    if (!parsed_limit.ok()) return core::make_unexpected(parsed_limit.error());
    // Rust `limit.unwrap_or(100)`. The ceiling is *not* clamped here: a
    // `limit` above 100 is a 400 from `query_build_logs`, so silently
    // reducing it would answer 200 with fewer entries than requested.
    if (parsed_limit.value().has_value()) {
        query.limit = static_cast<std::size_t>(*parsed_limit.value());
    }

    if (!level.empty()) {
        const core::Optional<core::LogRecordLevel> decoded =
            core::LogRecordLevelFromString(level);
        if (!decoded.has_value()) {
            return core::make_unexpected(
                core::err("invalid query parameter: unknown level '" + level + "'"));
        }
        query.level = decoded;
    }
    return query;
}

core::Expected<BuildLogQuery, core::AnyError> BuildLogQuery::ForLogs(
    const std::string& cursor, const std::string& limit, const std::string& level,
    const std::string& direction, const std::string& source) {
    // `limit` and `level` are the same parameters as on the status route, so
    // they are parsed by the same code. `logsOffset` has no spelling here --
    // this route pages by timestamp instead.
    core::Expected<BuildLogQuery, core::AnyError> query =
        BuildLogQuery::ForStatus(std::string(), limit, level);
    if (!query.ok()) return query;

    // Rust `query_params.cursor.map(i64::try_from)` with its own 400: the
    // schema types it `u64`, and a value past `i64::MAX` cannot be compared
    // against a timestamp at all.
    const core::Expected<core::Optional<int64_t>, core::AnyError> parsed_cursor =
        ParseQueryInt(cursor, "cursor");
    if (!parsed_cursor.ok()) return core::make_unexpected(parsed_cursor.error());
    query.value().cursor = parsed_cursor.value();

    // Rust `direction == Some(LogsDirectionBackward)` -- so `forward` and an
    // absent direction agree, but a *misspelled* one is refused rather than
    // read as forward: a client asking for `backwards` would otherwise get
    // the oldest page of a long log and no indication why.
    if (!direction.empty()) {
        if (direction == "backward") {
            query.value().backward = true;
        } else if (direction != "forward") {
            return core::make_unexpected(
                core::err("invalid query parameter: unknown direction '" + direction + "'"));
        }
    }

    if (!source.empty()) {
        if (source == "temporary") {
            query.value().source = core::Optional<LogsSource>(LogsSource::Temporary);
        } else if (source == "persistent") {
            query.value().source = core::Optional<LogsSource>(LogsSource::Persistent);
        } else {
            return core::make_unexpected(
                core::err("invalid query parameter: unknown source '" + source + "'"));
        }
    }
    return query;
}

core::Json BuildStatusReasonModel::ToJson() const {
    core::Json j;
    j["message"] = core::Json(message);
    // Both `skip_serializing_if = "Option::is_none"`. `step` in particular is
    // what the e2b SDKs parse as an integer index into their own recorded
    // steps, so writing `null` would break the error path of a client that
    // only checks for the key's presence.
    if (step.has_value()) j["step"] = core::Json(*step);
    if (log_entries.has_value()) {
        core::JsonArray entries;
        for (std::size_t i = 0; i < log_entries->size(); ++i) {
            entries.emplace_back((*log_entries)[i].ToJson());
        }
        j["logEntries"] = core::Json(entries);
    }
    return j;
}

TemplateBuildInfoModel TemplateBuildInfoModel::FromRecord(
    const snapshot::SnapshotRecord& record) {
    TemplateBuildInfoModel info;
    info.template_id = record.id.ToString();
    info.build_id    = record.id.ToString();
    info.status      = TemplateBuildStatusOf(record);

    // Rust only reads the reason out of the `Template` variant: a
    // sandbox-sourced record has no build and therefore no failure, and
    // reporting one would describe a captured snapshot as a failed build.
    if (record.source.is_template() && record.source.build.error_reason.has_value()) {
        BuildStatusReasonModel reason;
        reason.message = record.source.build.error_reason->message;
        reason.step    = record.source.build.error_reason->step;
        info.reason    = core::Optional<BuildStatusReasonModel>(reason);
    }
    return info;
}

core::Json TemplateBuildInfoModel::ToJson() const {
    core::Json j;
    // Required by the schema even though it is documented as always empty, so
    // a client reading `logs` finds an array rather than nothing.
    j["logs"] = core::Json(StringsToJson(logs));
    core::JsonArray entries;
    for (std::size_t i = 0; i < log_entries.size(); ++i) {
        entries.emplace_back(log_entries[i].ToJson());
    }
    j["logEntries"] = core::Json(entries);
    j["templateID"] = core::Json(template_id);
    j["buildID"]    = core::Json(build_id);
    // The lowercase wire spelling, not the record's capitalised one -- see
    // `TemplateBuildStatusWireName`.
    j["status"]     = core::Json(TemplateBuildStatusWireName(status));
    if (reason.has_value()) j["reason"] = reason->ToJson();
    return j;
}

// ---------------------------------------------------------------------------
// TemplateBuildStartBaseSource
// ---------------------------------------------------------------------------

TemplateBuildStartBaseSource TemplateBuildStartBaseSource::MakeDefaultImage() {
    return TemplateBuildStartBaseSource();
}

TemplateBuildStartBaseSource TemplateBuildStartBaseSource::MakeImage(const std::string& image) {
    TemplateBuildStartBaseSource source;
    source.kind_  = Image;
    source.image_ = image;
    return source;
}

TemplateBuildStartBaseSource TemplateBuildStartBaseSource::MakeTemplate(
    const snapshot::SnapshotAlias& alias) {
    TemplateBuildStartBaseSource source;
    source.kind_  = Template;
    source.alias_ = core::Optional<snapshot::SnapshotAlias>(alias);
    return source;
}

bool TemplateBuildStartBaseSource::operator==(const TemplateBuildStartBaseSource& o) const {
    if (kind_ != o.kind_) return false;
    switch (kind_) {
        case DefaultImage:
            return true;
        case Image:
            return image_ == o.image_;
        case Template:
            return alias_.has_value() && o.alias_.has_value() && *alias_ == *o.alias_;
    }
    return false;
}

namespace {

/// Rust `str::trim`.
std::string Trim(const std::string& value) {
    std::size_t begin = 0;
    while (begin < value.size() && (value[begin] == ' ' || value[begin] == '\t' ||
                                    value[begin] == '\n' || value[begin] == '\r')) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t' ||
                           value[end - 1] == '\n' || value[end - 1] == '\r')) {
        --end;
    }
    return value.substr(begin, end - begin);
}

std::string ToUpperAscii(const std::string& value) {
    std::string out = value;
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out[i] >= 'a' && out[i] <= 'z') out[i] = static_cast<char>(out[i] - 'a' + 'A');
    }
    return out;
}

/// Rust `.as_deref().map(str::trim).filter(|v| !v.is_empty())` — an absent
/// field and a whitespace-only one are treated the same.
core::Optional<std::string> TrimmedNonEmpty(const core::Optional<std::string>& value) {
    if (!value.has_value()) return core::Optional<std::string>();
    const std::string trimmed = Trim(*value);
    if (trimmed.empty()) return core::Optional<std::string>();
    return core::Optional<std::string>(trimmed);
}

const std::vector<std::string>& StepArgs(const TemplateStepRequest& step) {
    static const std::vector<std::string> kEmpty;
    return step.args.has_value() ? *step.args : kEmpty;
}

/// Rust's `args.first().filter(|v| !v.trim().is_empty())` — the first argument,
/// but only if it carries something.
core::Expected<std::string, ApiError> FirstNonBlankArg(const TemplateStepRequest& step,
                                                       const std::string& message) {
    const std::vector<std::string>& args = StepArgs(step);
    if (args.empty() || Trim(args[0]).empty()) {
        return core::make_unexpected(ApiError::Make(400, message));
    }
    // Note: the raw value is used, not the trimmed one — Rust only *tests* the
    // trimmed form and then clones the original.
    return args[0];
}

/// Rust's key/value loop over `args.chunks(2)`, shared by ENV/ARG and LABEL.
/// `min_args` differs: ENV/ARG reject an empty list via `args.is_empty()`,
/// LABEL via `args.len() < 2`, which amount to the same thing for an even
/// length, so one parameter covers both.
core::Expected<core::Unit, ApiError> ApplyPairs(tpl::TemplateBuildSpec* spec,
                                                const TemplateStepRequest& step, bool as_label,
                                                const std::string& pair_message,
                                                const std::string& key_message) {
    const std::vector<std::string>& args = StepArgs(step);
    if (args.empty() || args.size() % 2 != 0) {
        return core::make_unexpected(ApiError::Make(400, pair_message));
    }
    for (std::size_t i = 0; i + 1 < args.size(); i += 2) {
        const std::string key = Trim(args[i]);
        if (key.empty()) {
            return core::make_unexpected(ApiError::Make(400, key_message));
        }
        if (as_label) {
            spec->Label(key, args[i + 1]);
        } else {
            spec->Env(key, args[i + 1]);
        }
    }
    return core::Unit();
}

}  // namespace

// ---------------------------------------------------------------------------
// Step translation
// ---------------------------------------------------------------------------

core::Expected<core::Unit, ApiError> ApplyE2bTemplateStep(tpl::TemplateBuildSpec* spec,
                                                          const TemplateStepRequest& step) {
    // A blank hash means "no upload"; a real one means the client staged files
    // for a COPY, which would silently not happen if accepted here.
    if (step.files_hash.has_value() && !Trim(*step.files_hash).empty()) {
        return core::make_unexpected(ApiError::Make(
            400, "template build filesHash/COPY support is not implemented yet"));
    }

    const std::string type = ToUpperAscii(step.type);

    if (type == "RUN") {
        core::Expected<std::string, ApiError> cmd =
            FirstNonBlankArg(step, "RUN template step requires a command argument");
        if (!cmd.ok()) return core::make_unexpected(cmd.error());
        spec->Run(cmd.value());
        return core::Unit();
    }
    if (type == "ENV" || type == "ARG") {
        // The message quotes the *original* spelling, not the upper-cased one.
        return ApplyPairs(spec, step, false,
                          step.type + " template step requires key/value arguments",
                          step.type + " template step requires a non-empty key");
    }
    if (type == "WORKDIR") {
        core::Expected<std::string, ApiError> path =
            FirstNonBlankArg(step, "WORKDIR template step requires a path argument");
        if (!path.ok()) return core::make_unexpected(path.error());
        spec->Workdir(path.value());
        return core::Unit();
    }
    if (type == "USER") {
        core::Expected<std::string, ApiError> value =
            FirstNonBlankArg(step, "USER step requires a value");
        if (!value.ok()) return core::make_unexpected(value.error());
        spec->User(value.value());
        return core::Unit();
    }
    if (type == "EXPOSE") {
        core::Expected<std::string, ApiError> port =
            FirstNonBlankArg(step, "EXPOSE step requires a port argument");
        if (!port.ok()) return core::make_unexpected(port.error());
        spec->ExposedPort(port.value());
        return core::Unit();
    }
    if (type == "VOLUME") {
        core::Expected<std::string, ApiError> path =
            FirstNonBlankArg(step, "VOLUME step requires a path argument");
        if (!path.ok()) return core::make_unexpected(path.error());
        spec->Volume(path.value());
        return core::Unit();
    }
    if (type == "LABEL") {
        return ApplyPairs(spec, step, true, "LABEL step requires key/value arguments",
                          "LABEL step requires a non-empty key");
    }
    if (type == "COPY" || type == "ADD") {
        return core::make_unexpected(
            ApiError::Make(400, step.type + " template steps are not supported yet"));
    }
    // Rust interpolates the *raw* step type here, not the upper-cased one.
    return core::make_unexpected(
        ApiError::Make(400, "template step type " + step.type + " is not supported"));
}

// ---------------------------------------------------------------------------
// Request translation
// ---------------------------------------------------------------------------

core::Expected<sandbox::SandboxResources, ApiError> ResolveTemplateResources(
    const TemplateBuildRequestV3& body) {
    const cfg::AppConfig* config = cfg::ConfigManager::GlobalConfig();

    // Rust reads the global config unconditionally (it panics if absent). Here
    // a missing global falls back to the same literals `MachineConfig`
    // declares, so a caller that never initialised the config still gets the
    // documented defaults rather than a crash.
    uint32_t cpu_count  = config != NULL ? config->machine.vcpu_count : 2;
    uint32_t memory_mib = config != NULL ? config->machine.mem_size_mib : 1024;
    if (body.cpu_count.has_value()) cpu_count = *body.cpu_count;
    if (body.memory_mb.has_value()) memory_mib = *body.memory_mb;

    if (cpu_count == 0 || memory_mib == 0) {
        return core::make_unexpected(
            ApiError::Make(400, "cpuCount and memoryMB must be greater than 0"));
    }

    sandbox::SandboxResources resources;
    resources.cpu_count  = cpu_count;
    resources.memory_mib = memory_mib;
    // A template build sizes its disk from the image, not from the request.
    resources.disk_size_mib = 0;
    return resources;
}

core::Expected<snapshot::SnapshotRecord, ApiError> TemplateBuildRecordFromV3Request(
    const TemplateBuildRequestV3& body, const core::SnapshotId& id, const std::string& alias) {
    // A `name:tag` reference would silently build the untagged template.
    if (alias.find(':') != std::string::npos) {
        return core::make_unexpected(
            ApiError::Make(400, "template name tags are not supported yet"));
    }
    if (body.tags.has_value() && !body.tags->empty()) {
        return core::make_unexpected(ApiError::Make(400, "template tags are not supported yet"));
    }

    core::Expected<snapshot::SnapshotAlias, std::string> parsed =
        snapshot::SnapshotAlias::Parse(alias);
    if (!parsed.ok()) {
        return core::make_unexpected(ApiError::Make(400, parsed.error()));
    }

    core::Expected<sandbox::SandboxResources, ApiError> resources = ResolveTemplateResources(body);
    if (!resources.ok()) return core::make_unexpected(resources.error());

    return snapshot::SnapshotRecord::TemplateWaiting(
        id, core::Optional<snapshot::SnapshotAlias>(parsed.value()), resources.value());
}

core::Expected<tpl::TemplateBuildSpec, ApiError> TemplateBuildSpecFromStartRequest(
    const TemplateBuildStartRequest& body, const core::Optional<snapshot::SnapshotAlias>& alias,
    const sandbox::SandboxResources& resources) {
    tpl::TemplateBuildSpec spec;
    spec.Resources(resources.cpu_count, resources.memory_mib);
    if (alias.has_value()) spec.Alias(alias->ToString());
    if (body.start_cmd.has_value()) spec.StartCmd(*body.start_cmd);
    if (body.ready_cmd.has_value()) spec.ReadyCmd(*body.ready_cmd);

    if (body.steps.has_value()) {
        const std::vector<TemplateStepRequest>& steps = *body.steps;
        for (std::size_t index = 0; index < steps.size(); ++index) {
            // One client step can expand into several internal steps, so the
            // range it produced is stamped with the client's 1-based number.
            // Without this, a multi-pair ENV would shift every later step's
            // reported position.
            const std::size_t appended_from = spec.StepCount();
            core::Expected<core::Unit, ApiError> applied =
                ApplyE2bTemplateStep(&spec, steps[index]);
            if (!applied.ok()) return core::make_unexpected(applied.error());
            spec.StampSourceSteps(appended_from, index + 1);
        }
    }

    return spec;
}

core::Expected<TemplateBuildStartBaseSource, ApiError> TemplateBuildStartBaseSourceFrom(
    const TemplateBuildStartRequest& body) {
    const core::Optional<std::string> from_image    = TrimmedNonEmpty(body.from_image);
    const core::Optional<std::string> from_template = TrimmedNonEmpty(body.from_template);

    if (from_image.has_value() && from_template.has_value()) {
        return core::make_unexpected(
            ApiError::Make(400, "cannot specify both fromImage and fromTemplate"));
    }
    if (from_image.has_value()) {
        return TemplateBuildStartBaseSource::MakeImage(*from_image);
    }
    if (from_template.has_value()) {
        core::Expected<snapshot::SnapshotAlias, std::string> parsed =
            snapshot::SnapshotAlias::Parse(*from_template);
        if (!parsed.ok()) {
            return core::make_unexpected(ApiError::Make(400, parsed.error()));
        }
        return TemplateBuildStartBaseSource::MakeTemplate(parsed.value());
    }
    return TemplateBuildStartBaseSource::MakeDefaultImage();
}

}  // namespace api
}  // namespace agentenv
