"""
server_encrypted_compute.py - Run homomorphic computation on Lattica server
"""
import json
import pickle
import sys
from lattica_query import performance_utils
import submission_utils

local_file_paths, _ = submission_utils.init(sys.argv)
# Load encrypted query from step 8
ct = pickle.load(open(local_file_paths.get_ct_upload_path("query"), "rb"))

# Run homomorphic computation
client = submission_utils.get_lattica_client(local_file_paths)
ct_res = client.worker_api.apply_hom_pipeline(ct, return_new_state=True)

# Save encrypted result
pickle.dump(ct_res, open(local_file_paths.get_ct_download_path("results"), "wb"))

# Parse and save server timing report
server_timing = client.worker_api.get_last_timing()
server_report_for_harness = performance_utils.server_timing_report(server_timing)
with open(local_file_paths.SERVER_TIMES_PATH, "w") as f:
    json.dump(server_report_for_harness, f)
