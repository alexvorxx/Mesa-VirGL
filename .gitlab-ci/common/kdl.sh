#!/usr/bin/env bash
# shellcheck disable=SC1091  # the path is created in build-kdl and
# here is check if exist

terminate() {
  echo "ci-kdl.sh caught SIGTERM signal! propagating to child processes"
  for job in $(jobs -p)
  do
    kill -15 "$job"
  done
}

trap terminate SIGTERM

if [ -f /ci-kdl/bin/activate ]; then
  source /ci-kdl/bin/activate
  /ci-kdl/bin/python /ci-kdl/bin/ci-kdl | tee -a "$RESULTS_DIR/kdl.log" &
  child=$!
  wait $child
  mv kdl_*.json "$RESULTS_DIR/kdl.json"
else
  echo -e "Not possible to activate ci-kdl virtual environment"
fi

