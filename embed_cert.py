# embed_cert.py
import os

def before_build(source, target, env):
    ca_path = "src/certs/ca_cert.pem"
    out_path = os.path.join(env.subst("$BUILD_DIR"), "embedded_ca_cert.c")

    with open(ca_path, "r") as f:
        pem = f.read()

    # convert to C string literal
    cert_lines = ['"%s\\n"' % line.strip().replace('"', '\\"') for line in pem.strip().splitlines()]
    cert_array = "\n".join(cert_lines)

    with open(out_path, "w") as f:
        f.write('#include <stdio.h>\nconst char* ca_cert_pem =\n%s;\n' % cert_array)

    env.Append(SRC_FILTER=["+<%s>" % out_path])
