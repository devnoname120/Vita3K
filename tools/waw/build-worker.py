from pathlib import Path
import datetime
import json
import os
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
STATUS = ROOT / "build/waw-checks/build-job.json"
LOG = ROOT / "build/waw-checks/xcode-build-persistent.log"
command = ["xcodebuild", "-project", "build/macos-xcode/Vita3K.xcodeproj",
           "-target", "vita3k", "-configuration", "RelWithDebInfo", "-jobs", "12",
           "MACOSX_DEPLOYMENT_TARGET=13.0", "ARCHS=arm64", "ONLY_ACTIVE_ARCH=YES",
           "CODE_SIGNING_ALLOWED=NO", "CODE_SIGNING_REQUIRED=NO"]
state = {"pid": os.getpid(), "command": command, "cwd": str(ROOT),
         "started": datetime.datetime.now(datetime.timezone.utc).isoformat(),
         "log": str(LOG), "status": "running"}

def save():
    temporary = STATUS.with_suffix(".pending")
    temporary.write_text(json.dumps(state, indent=2) + "\n")
    temporary.replace(STATUS)

save()
try:
    with LOG.open("w") as output:
        result = subprocess.run(command, cwd=ROOT, stdin=subprocess.DEVNULL,
                                stdout=output, stderr=subprocess.STDOUT)
    state.update(status="finished", exit_code=result.returncode)
except Exception as error:
    state.update(status="failed", error=repr(error))
finally:
    state["ended"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    save()
