#include "poly/json.hpp"
#include "testing.hpp"

using poly::json::Value;
using poly::json::Writer;

namespace {
Value parse_ok(const char* text) {
  Value v;
  std::string error;
  if (!poly::json::parse(text, v, error)) {
    ::poly::testing::report_failure(__FILE__, __LINE__, std::string("parse failed: ") + error);
  }
  return v;
}
}  // namespace

TEST("json: parses the shapes rule data uses") {
  const Value v = parse_ok(R"({
    "version": "live-2.16.3",
    "combat": {"accelerator": 4.5, "wall": 4},
    "units": {"warrior": {"cost": 2, "attack": 2.0, "skills": ["dash", "fortify"]}},
    "_unverified": ["city.besieged"],
    "missing": null,
    "flag": true
  })");

  CHECK(v.is_object());
  CHECK_EQ(v["version"].str(), std::string("live-2.16.3"));
  CHECK(v["combat"]["accelerator"].num() == 4.5);
  CHECK_EQ(v["combat"]["wall"].integer(), 4);
  CHECK_EQ(v["units"]["warrior"]["cost"].integer(), 2);
  CHECK_EQ(static_cast<int>(v["units"]["warrior"]["skills"].arr().size()), 2);
  CHECK_EQ(v["units"]["warrior"]["skills"].arr()[0].str(), std::string("dash"));
  CHECK(v["missing"].is_null());
  CHECK(v["flag"].boolean());
}

TEST("json: absent keys fall back instead of throwing") {
  // Rule data is legitimately incomplete -- unknown values are null by design (the
  // "_unverified" list). A missing key must be a recoverable default, never a startup crash.
  const Value v = parse_ok(R"({"a": 1})");
  CHECK_EQ(v["nope"].integer(7), 7);
  CHECK_EQ(v["nope"]["deeper"]["deeper still"].integer(42), 42);
  CHECK(v["nope"].is_null());
  CHECK(!v.contains("nope"));
  CHECK(v.contains("a"));
  // Wrong-type access also falls back rather than reinterpreting.
  CHECK_EQ(v["a"].str(), std::string(""));
}

TEST("json: negative and fractional numbers survive") {
  // Both matter here: population goes negative (R17) and unit stats are fractional
  // (Knight attack 3.5, Cloak defence 0.5).
  const Value v = parse_ok(R"({"pop": -9, "atk": 3.5, "def": 0.5, "exp": 1e3})");
  CHECK_EQ(v["pop"].integer(), -9);
  CHECK(v["atk"].num() == 3.5);
  CHECK(v["def"].num() == 0.5);
  CHECK(v["exp"].num() == 1000.0);
}

TEST("json: malformed input is rejected with a line number") {
  const char* bad[] = {
      "{",  "{\"a\": }", "{\"a\" 1}", "[1, 2", "{\"a\": 1,}", "tru", "{} extra",
  };
  for (const char* text : bad) {
    Value v;
    std::string error;
    const bool ok = poly::json::parse(text, v, error);
    CHECK(!ok);
    CHECK(!error.empty());
  }
}

TEST("json: writer emits empty containers correctly at depth") {
  // The exact case a last-character heuristic gets wrong: an empty object inside an array
  // would emit `[{},]`. This is why the writer tracks a container stack.
  Writer w;
  w.begin_array();
  w.begin_object();
  w.end_object();
  w.begin_object();
  w.end_object();
  w.end_array();
  CHECK_EQ(w.text(), std::string("[{},{}]"));
}

TEST("json: writer round-trips a state-view-shaped document") {
  Writer w;
  w.begin_object();
  w.field("turn", 3);
  w.field("current_player", 0);
  w.key("tiles");
  w.begin_array();
  w.begin_object();
  w.field("i", 47);
  w.field("terrain", "field");
  w.end_object();
  w.end_array();
  w.key("cities");
  w.begin_array();
  w.begin_object();
  w.field("population", -9);  // negative populations must survive the wire
  w.field("hp", 5.5);
  w.end_object();
  w.end_array();
  w.key("outcome");
  w.value(nullptr);
  w.end_object();

  Value v;
  std::string error;
  const bool ok = poly::json::parse(w.text(), v, error);
  CHECK(ok);
  CHECK_EQ(v["turn"].integer(), 3);
  CHECK_EQ(v["tiles"].arr()[0]["i"].integer(), 47);
  CHECK_EQ(v["cities"].arr()[0]["population"].integer(), -9);
  CHECK(v["cities"].arr()[0]["hp"].num() == 5.5);
  CHECK(v["outcome"].is_null());
}

TEST("json: writer escapes strings") {
  Writer w;
  w.begin_object();
  w.field("q", "he said \"hi\"\n\tand left\\");
  w.end_object();

  Value v;
  std::string error;
  CHECK(poly::json::parse(w.text(), v, error));
  CHECK_EQ(v["q"].str(), std::string("he said \"hi\"\n\tand left\\"));
}

TEST("json: non-finite doubles become null rather than invalid JSON") {
  // NaN and Infinity are not JSON. Emitting them literally produces a document the
  // browser's own JSON.parse rejects, which shows up as an unexplained blank screen.
  Writer w;
  w.begin_object();
  w.field("nan", 0.0 / 0.0);
  w.field("inf", 1.0 / 0.0);
  w.end_object();

  Value v;
  std::string error;
  CHECK(poly::json::parse(w.text(), v, error));
  CHECK(v["nan"].is_null());
  CHECK(v["inf"].is_null());
}
