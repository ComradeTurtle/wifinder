#!/usr/bin/env bash
set -euo pipefail

gcc -std=c11 -Wall -Wextra -Werror -I./main \
  tests/test_sniffer_logic.c main/sniffer_logic.c \
  -o /tmp/test_sniffer_logic

gcc -std=c11 -Wall -Wextra -Werror -I./main \
  tests/test_channel_plan.c main/channel_plan.c \
  -o /tmp/test_channel_plan

gcc -std=c11 -Wall -Wextra -Werror -I./main \
  tests/test_wg_protocol.c main/wg_protocol.c \
  -o /tmp/test_wg_protocol

gcc -std=c11 -Wall -Wextra -Werror -I./main \
  tests/test_wg_payload.c main/wg_payload.c \
  -o /tmp/test_wg_payload

/tmp/test_sniffer_logic
/tmp/test_channel_plan
/tmp/test_wg_protocol
/tmp/test_wg_payload
