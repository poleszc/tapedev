#!/bin/sh

for t in /dev/tapedev*; do
    dev=$(basename $t)
    echo $dev
    echo type
    cat /sys/block/$dev/tape/tape_type 
    echo tapes
    cat /sys/block/$dev/tape/tapes
    echo current tape
    cat /sys/block/$dev/tape/current_tape
    echo
done

