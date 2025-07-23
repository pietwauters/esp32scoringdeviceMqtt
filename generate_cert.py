import os

# Paths
INPUT = "certs/ca_cert.pem"  # Input certificate file (PEM format)
OUTPUT = "src/ca_cert.c"     # Output C file

VAR_NAME = "ca_cert_pem"     # Variable name for the certificate array

# Ensure the input file exists
if not os.path.exists(INPUT):
    print(f"Error: File {INPUT} not found.")
    exit(1)

# Read the certificate in binary mode to preserve it as-is
with open(INPUT, "rb") as f:
    pem_data = f.read()

# Write the C file with the embedded certificate
with open(OUTPUT, "w") as out:
    out.write('#include <stddef.h>\n\n')
    out.write(f'const char {VAR_NAME}[] =\n')

    # Write each byte as a C character literal, properly escaped
    for byte in pem_data:
        # Convert each byte into a readable escape format
        if 32 <= byte <= 126 and byte not in (34, 92):  # Exclude " and \
            out.write(f'"{chr(byte)}"')
        elif byte == 10:  # Newline byte in PEM format
            out.write('"\\n"')
        else:  # For other non-printable characters, use hex escape
            out.write(f'"\\x{byte:02x}"')
        out.write('\n')

    out.write(';\n')
    out.write(f'const size_t {VAR_NAME}_len = sizeof({VAR_NAME}) - 1;\n')

print(f"Certificate embedded in {OUTPUT} successfully.")
