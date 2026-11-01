"""Offline Draft 7 validation for source fixtures and C++ serialized outputs."""
import copy
import json
import sys
from pathlib import Path
from jsonschema import Draft7Validator

contracts, generated = map(Path, sys.argv[1:])
validators = {}
for kind in ("request", "manifest", "report", "event"):
    schema = json.loads((contracts / (kind + ".schema.json")).read_text())
    Draft7Validator.check_schema(schema)
    validators[kind] = Draft7Validator(schema)

def load(path):
    return json.loads(path.read_text())

count = 0
for directory in (contracts / "fixtures", generated):
    for path in directory.glob("*.json"):
        kind = "report" if path.name.startswith("report-") else path.stem
        validators[kind].validate(load(path))
        count += 1
for line in (contracts / "fixtures/events.jsonl").read_text().splitlines():
    validators["event"].validate(json.loads(line))
    count += 1
assert count == 13, ("missing expected fixtures", count)

def reject(kind, value):
    assert not validators[kind].is_valid(value), ("unexpectedly valid", kind, value)

request = load(contracts / "fixtures/request.json")
for field, value in [("parameters", []), ("timeout_sec", -1), ("max_output_bytes", -1),
                     ("require_confirm", "true"), ("input_snapshot_id", ""),
                     ("schema_version", "future")]:
    bad = copy.deepcopy(request); bad[field] = value; reject("request", bad)
bad = copy.deepcopy(request); del bad["project_id"]; reject("request", bad)
report = load(contracts / "fixtures/report-success.json")
for path in ["/tmp/out.json", "../out.json", "artifacts/../../out.json"]:
    bad = copy.deepcopy(report); bad["artifacts"][0]["path"] = path; reject("report", bad)
bad = copy.deepcopy(report); bad["artifacts"][0]["sha256"] = "bad"; reject("report", bad)
bad = copy.deepcopy(report); bad["exit_code"] = "0"; reject("report", bad)
bad = copy.deepcopy(report); bad["state"] = "success"; reject("report", bad)
reject("event", {"type":"result", "state":"Running", "exitCode":0, "jobId":"job-001"})
reject("event", {"type":"result", "state":"Succeeded", "exit_code":0, "job_id":"job-001"})
reject("event", {"type":"progress", "percent":101, "status":"Running"})
# Old metrics and diagnostics are provider-owned JSON, including null/arrays.
for value in (None, [], {}, [{"exit_code":0}]):
    candidate=copy.deepcopy(report); candidate["metrics"]=value; candidate["diagnostics"]=value
    validators["report"].validate(candidate)
print(f"Validated {count} fixtures/generated documents and negative contract cases")
