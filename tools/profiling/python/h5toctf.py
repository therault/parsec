#!/usr/bin/env python3

try:
    import os
    import numpy as np
    import time
    import pandas
    import sys
except ModuleNotFoundError:
    print("Did not find a system module, use pip to install it")

try:
    import parsec_trace_tables as ptt
    import pbt2ptt
except ModuleNotFoundError:
    print("Did not find pbt2ptt, you are likely using python version that does not match the version used to build PaRSEC profiling tools")
    print(sys.path)

import json
import re
import sys
import math
import argparse

def bool(str):
    return str.lower() in ["true", "yes", "y", "1", "t"]

def h5_to_ctf(ptt_filename, ctf_filename, **kwargs):
    print(f"Converting {ptt_filename} into {ctf_filename}")

    skip_parsec_events = bool(kwargs.get('skip_parsec_events', True))
    skip_mpi_events = bool(kwargs.get('skip_mpi_events', True))
    key_is_part_of_task_name = bool(kwargs.get('key_is_part_of_task_name', False))

    ctf_data = {"traceEvents": []}

    trace = ptt.from_hdf(ptt_filename)

    for e in trace.events.itertuples():
        if(skip_parsec_events == True and trace.event_names[e.type].startswith("PARSEC")):
            continue
        if(skip_mpi_events == True and trace.event_names[e.type].startswith("MPI")):
            continue

        ctf_event = {}
        ctf_event["ph"] = "X"  # complete event type
        ctf_event["ts"] = 0.001 * e.begin # when we started, in ms
        ctf_event["dur"] = 0.001 * (e.end - e.begin) # when we started, in ms
        category = trace.event_names[e.type]
        ctf_event["cat"] = category

        if e.key is not None:
            key = e.key.decode('utf-8').rstrip('\x00')
            ctf_event["args"] = { "key": key }
            if key_is_part_of_task_name:
                ctf_event["name"] = category+"<"+key+">"
            else:
                ctf_event["name"] = category
        else:
            ctf_event["name"] = category

        ctf_event["pid"] = e.node_id
        tid = trace.streams.th_id[e.stream_id]
        ctf_event["tid"] = 111111 if math.isnan(tid) else int(tid)

        ctf_data["traceEvents"].append(ctf_event)

    with open(ctf_filename, "w") as chrome_trace:
        json.dump(ctf_data, chrome_trace)

if __name__ == "__main__":

    parser = argparse.ArgumentParser( prog='h5toctf',
                                      description='Convert a HDF5 PaRSEC profile file into a Perfetto profiling in JSON format')
    parser.add_argument('ptt_file_name', nargs=1, action='store', help='PaRSEC HDF5 profile file')
    parser.add_argument('ctf_file_name', nargs=1, action='store', help='Name of the generated JSON file')
    parser.add_argument('--show-parsec-events', action='store_const', const='False', default='True', dest='skip_parsec_events', help='Include internal PaRSEC events in the trace' )
    parser.add_argument('--show-mpi-events', action='store_const', const='False', default='True', dest='skip_mpi_events', help='Include internal MPI events (generated by the PaRSEC internal comm engine) in the trace' )
    parser.add_argument('--key-is-part-of-task-name', action='store_const', const='True', default='False', dest='key_is_part_of_task_name', help='Include the key in the task name' )
    args = parser.parse_args()

    h5_to_ctf(args.ptt_file_name[0], args.ctf_file_name[0], **vars(args))
