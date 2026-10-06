#pragma once
// JSON (de)serialization of catalog models (library.json / session.json and other stores). Only needs catalog +
// nlohmann, so tests can compile it directly.
#include "catalog/Models.h"

#include <nlohmann/json.hpp>

namespace st::app {

nlohmann::json toJson(const catalog::Track& t);
nlohmann::json toJson(const catalog::Album& a);
nlohmann::json toJson(const catalog::Artist& a);
catalog::Track trackFromJson(const nlohmann::json& j);
catalog::Album albumFromJson(const nlohmann::json& j);
catalog::Artist artistFromJson(const nlohmann::json& j);

} // namespace st::app
