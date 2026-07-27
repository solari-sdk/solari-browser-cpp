#include "solari/browser/types.hpp"

namespace solari::browser {

Json proxyToJson(const ProxySpec& spec) {
  // Shorthand form: a country ("us"), or the "off" / "smart" sentinels.
  if (std::holds_alternative<std::string>(spec)) {
    return Json(std::get<std::string>(spec));
  }
  const ProxyRequest& p = std::get<ProxyRequest>(spec);
  Json j = Json::object();
  if (p.country) j["country"] = *p.country;
  if (p.tier) j["tier"] = *p.tier;
  if (p.asn) j["asn"] = *p.asn;
  if (p.session) j["session"] = *p.session;
  if (p.sessionDuration) j["sessionDuration"] = *p.sessionDuration;
  if (p.state) j["state"] = *p.state;
  if (p.city) j["city"] = *p.city;
  return j;
}

}  // namespace solari::browser
