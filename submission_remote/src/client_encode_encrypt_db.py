"""
client_encode_encrypt_db.py - Encrypt the database
"""
import zipfile
from lattica_query.serialization.api_serialization_utils import dumps_proto_tensor
import pickle
import sys

import lattica_query.query_toolkit as toolkit_interface
import submission_utils

local_file_paths, _ = submission_utils.init(sys.argv)

# Read data from local filesystem required for encoding and encrypting
context = pickle.load(open(local_file_paths.PATH_CONTEXT, "rb"))
sk =      pickle.load(open(local_file_paths.PATH_SK,      "rb"))

# Load the db and payloads from step 2
# db_tensor       = torch.from_numpy(np.load(local_file_paths.DB_PATH))
# payloads_tensor = torch.from_numpy(np.load(local_file_paths.PAYLOAD_PATH))
db =      pickle.load(open(local_file_paths.PROCESSED_DB_PATH,      "rb"))
payload = pickle.load(open(local_file_paths.PROCESSED_PAYLOAD_PATH, "rb"))

# Encrypt db using Lattica toolkit
encrypted_db_data = toolkit_interface.enc(
    context,
    sk,
    dumps_proto_tensor(db),
    custom_state_name="db",
)

# Encrypt payloads using Lattica toolkit
encrypted_payloads_data = toolkit_interface.enc(
    context,
    sk,
    dumps_proto_tensor(payload),
    custom_state_name="payloads",
)

# Write archive containing db & payloads
archive_path = local_file_paths.get_ct_upload_path("db_and_payloads_zip")
with zipfile.ZipFile(archive_path, 'w') as zipf:
    zipf.writestr('db.bin', encrypted_db_data)
    zipf.writestr('payloads.bin', encrypted_payloads_data)
