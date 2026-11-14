#!/bin/sh
# Example PNR plugin (SDK demo). Talks to the host with one JSON object per line on stdout; the host answers on stdin.
echo '{"call":"log","msg":"hello-process starting"}'; read _ack
echo '{"call":"library_list"}'; read games
echo '{"call":"data_put","name":"last-run.txt","value":"ran once"}'; read _ok
n=$(echo "$games" | grep -o '"id"' | wc -l)
echo '{"call":"result","value":"library has '"$n"' game(s)"}'
