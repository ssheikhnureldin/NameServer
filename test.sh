#!/bin/bash

# Run the command
./ns server_map1.csv & sleep 1 ; echo "hello world" | ./req echo

# Check the exit code
if [ $? -eq 0 ]; then
  echo "Test succeeded!"
else
  echo "Test failed :("
fi