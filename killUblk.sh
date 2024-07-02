#! /usr/bin/bash

for pid in `pidof longhorn`
do
	kill -9 $pid
done

for pid in `pidof fio`
do
        kill -9 $pid
done

for pid in `pidof ublk`
do
        kill -9 $pid
done
ublk del -a
