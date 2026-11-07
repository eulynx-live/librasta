// Generates the CA certificate, server certificate and server key that the
// TLS/DTLS transport tests load. Registered as a CTest fixture so the tests
// work in a fresh build directory without running the rcat examples first.

#include <stdio.h>
#include <unistd.h>

#include "wolfssl_certificate_helper.h"

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <ca_cert_path> <server_cert_path> <server_key_path>\n", argv[0]);
        return 1;
    }

    // do not overwrite existing certificates, they might be in use by the rcat examples
    if (access(argv[1], F_OK) == 0 && access(argv[2], F_OK) == 0 && access(argv[3], F_OK) == 0) {
        return 0;
    }

    create_certificates(argv[1], argv[2], argv[3]);

    if (access(argv[1], F_OK) || access(argv[2], F_OK) || access(argv[3], F_OK)) {
        fprintf(stderr, "Failed to generate certificates\n");
        return 1;
    }
    return 0;
}
