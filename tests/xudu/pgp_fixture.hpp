/**
 * @file pgp_fixture.hpp
 * @brief Real OpenPGP keys and detached signatures, for the verification tests.
 *
 * Generated once with GnuPG and checked in rather than produced at test time:
 * a test that mints its own key pair proves the library agrees with itself,
 * where these prove it agrees with an independent implementation of the same
 * standard. Both keys are throwaway RSA-2048 with no passphrase and a
 * .invalid address, and neither signs anything outside this file.
 *
 * kAuthorMessage is what kAuthorSignature signs. kImpostorSignature is a
 * genuine signature over the same bytes by a different key, which is the case
 * that separates "this signature is well formed" from "this signature is by
 * the key we require".
 */
#ifndef XUDU_TESTS_PGP_FIXTURE_HPP
#define XUDU_TESTS_PGP_FIXTURE_HPP

#include <string_view>

namespace xudu::testing {

inline constexpr std::string_view kAuthorFingerprint =
    "2151B04ADF99D0AB9886CCD5AB83CC2E0A1D80F0";

inline constexpr std::string_view kImpostorFingerprint =
    "9D7EAE7DA54B600656115C0591F07DD3C211514A";

inline constexpr std::string_view kAuthorMessage =
    "device-delegation-canonical-bytes";

inline constexpr std::string_view kAuthorPublicKey = R"PGP(
-----BEGIN PGP PUBLIC KEY BLOCK-----

mQENBGqYKN0BCACnm+1XcUpJwajYYyEW8HLFHzeKYu8vCmKTeMQbNtbaRQqKVgU/
1NC2oWLpMVYmxPQsPdM5F6mPBLwt3J/W6J647hWBd2T5SQOOj7+4M5jsmczlFpMw
MpiQpEnVLEBwOcGOTnMtEWlxs37oHcB7Kg8vZU80LrOuQKUvzNdXOUCl1kI+LKsg
E8SZCz9Ljb/1dP/g897g67lTKPOCdWi5RuTqZZPf2/1DqAizUiIWtDsHXed8aOWx
ZRqkdiQDJzjkG+av15hGB0G4mcPeKFh0h/xm523McGRTVV2w6FrWLh5OE+8UB6oD
BRurTnsJmIj4qONVV3ybRh3PEWcbQfYD7EHpABEBAAG0Jlh1ZHUgVGVzdCBBdXRo
b3IgPGF1dGhvckB0ZXN0LmludmFsaWQ+iQFOBBMBCgA4FiEEIVGwSt+Z0KuYhszV
q4PMLgodgPAFAmqYKN0CGwMFCwkIBwIGFQoJCAsCBBYCAwECHgECF4AACgkQq4PM
LgodgPBKcAf+OTuNA2Nz+eOExW2wXhm66u59yoL0SJ0Kz+4rqGoooW1TtAL1zoJ/
Lg7AGaIToxdq3C0MWQ81fYrhiEReT/cxr9VJ2xXpiCQk3+MPOGq3s9omUmif7EAm
ktQh595rwNIkBkHU/CJADs33bYDfrkK21bYHJVRHYnaBfuI7Q93xfJwpDL/wWP3o
gK6lfj7Kc/TyNRuNGVes0Tarn/JS66H88uah+C44pz6Af8X8gnslUKZYSIw1yvAX
52IdZix0wFEWl6x8kf5YGG35P9oQw1DkQ6Ec6XFh6QW8Nnhlok8GziS9TJ4aS0W5
KkcfA9mKBLULbrNsJoZHYJHRU3n+cLIzGw==
=4SkB
-----END PGP PUBLIC KEY BLOCK-----
)PGP";

inline constexpr std::string_view kImpostorPublicKey = R"PGP(
-----BEGIN PGP PUBLIC KEY BLOCK-----

mQENBGqYKPYBCADlg2Pkjr/68ES9mEYWjEa8KjWbtOzY/JhBVb3Y014i1HNFkh/U
IvPApzmFQLjrAUk8ZKDkvs0AUVfN3XHBTb1fWZB4l7i5/fP9dflYynf+itAlQkqz
V8YEXSXaG5x+UcMXTSaedutXvYog86A7fxxk7n5c/V5UGKZggsZ1aT5wbgeFTBGQ
Rv+96mFnYq90nUfvmy28i+15CLYlrwYE+mf4/3SId5epVu0ZxkxcXckChpaI6sXq
ueG7C/OlezwiEa4rC2gS0qPtp//0Yb8DkFKK3UbI/TOFJ2gXivKYFj8EQ6CtxTXF
Ns1FGj9tfB+gCv79M0CDSyYYVrAT50fsdYr5ABEBAAG0JVh1ZHUgSW1wb3N0b3Ig
PGltcG9zdG9yQHRlc3QuaW52YWxpZD6JAU4EEwEKADgWIQSdfq59pUtgBlYRXAWR
8H3TwhFRSgUCapgo9gIbAwULCQgHAgYVCgkICwIEFgIDAQIeAQIXgAAKCRCR8H3T
whFRSklUB/96E4Yafm9qlvScPl8uAfLu2mh3+Tmytfmv+zrvFo2Tjgl+4Nk1GkSa
JeJGLQ49psfvOKrkHEXldp3svi3ImZRV9QOJSdtGY8m24ue8LYpzz8NdekXK3U5W
QkP1GFRU8HwPuj+/MsHUQSbcS6mQ3sjARaK/9ai218anfNPMcR/G5IOrYq5bKsIx
mWGcP3lekP3wm22oYJiS1Qdx4mMWUgt5y65nVAgDrVGvbTMzhBGnXsm+MoKjYXaZ
xV9LEN0V2bdESC6GUIDe+XJ5IuEaRnYhSbgnJwjZDsw8T90y8onBASm4VL89qABJ
q+9DYJjSZulg29trHikKfqjW3DXOv/42
=2gFw
-----END PGP PUBLIC KEY BLOCK-----
)PGP";

inline constexpr std::string_view kAuthorSignature = R"PGP(
-----BEGIN PGP SIGNATURE-----

iQEzBAABCgAdFiEEIVGwSt+Z0KuYhszVq4PMLgodgPAFAmqYKN0ACgkQq4PMLgod
gPCJ5AgAo52CMw+C/M7tnEiHrT/VcisYQwSQRzPSG+/1mUg/RTpDis+nuLvVbWqT
xioFL8jUSTh4ZN/Q2U7D1lUQzlYlJbMIYzoRcVzxfa0Qf43nnBEXVlxeo4ajbYcW
nCsCeEBm9WgxEJIUbFxcIXLc6nYtNrABg3Lmov3ybWjymU2bhFDsit2lfMv244E4
WBCOMDC3t/7Z3P+MGyuZaMauxtW1Ko6A7jYJFffIOV2M2GN3/uulccSpvoc226My
Lf7GL3Z74XZ8Gv2mykzbzdunPr1yLLJt8ZGDa4wszdnEk5BOcaJ/eYCSXY4G7L3f
ECGunCc94wPvn6WctJG9mcHOEi6KNw==
=sSEg
-----END PGP SIGNATURE-----
)PGP";

inline constexpr std::string_view kImpostorSignature = R"PGP(
-----BEGIN PGP SIGNATURE-----

iQEzBAABCgAdFiEEnX6ufaVLYAZWEVwFkfB908IRUUoFAmqYKPcACgkQkfB908IR
UUpp0Qf9HTnxInPvO+gfcH0yZx69DpN4RGA/qbtDzRdWH9dfwa59hbGvp3iFqYLk
dJt1qItkbfGAJsbm7gc8jjxRLgdvE34+qAhfewJsa0CSBo6qtNvt2N1buSjqYV3/
6EpWzbl0NlMK9Sjx3kvzYAIG2EztGSkzc158U0L/q+F6LBzaN2Z1AIO+ss1/1UfT
DysdRs3wXk7V4dHnvkAFry6tCSCbj9446ClgKSZW+GRPrH7Ij4vdFAGYC+r51QBy
mWEi8wQdZIlT0rUfeBdrL2WXgKm9OnXKXFsCIggZ+GJtAIgd3sh7AwMEhWrFvJs1
WUlr2GlrbP53Q6tM+ZfdQqX6ZrqoZQ==
=Xf4q
-----END PGP SIGNATURE-----
)PGP";

/// A delegation of the all-0x11 device key to the author's master key,
/// signed by GnuPG over exactly the bytes DeviceDelegation::signingBuffer()
/// produces for it. Regenerate if that format ever changes -- the point of
/// the fixture is that an outside implementation agrees with ours.
inline constexpr std::string_view kDeviceDelegationSignature = R"PGP(
-----BEGIN PGP SIGNATURE-----

iQEzBAABCgAdFiEEIVGwSt+Z0KuYhszVq4PMLgodgPAFAmqYKfMACgkQq4PMLgod
gPCAqwf+Oz5fnG/7TZ43U/u15KmNh4Ul5nfi56CPYTIxZOJYQEoissG3k1Vfal/r
zi3EcU2Oqf0DcMQsFIvsCCA7nzsFzavc89m8GMjndANUHFD8wE24J/JRE8qa/SGQ
3h4p7EBfCtQkRGPAoBALp59q4fT2rIG/k3Pcw6dv2aJiLu46fptZNl0KvloomGB/
ZtFlLB+bENgrmC5jvpvTDS28LSRaBJQLN4Ow+ajNhNcLyg6LOBQJ6cwJVCMbnSQg
/+BsW0u6QthdxR4AvCUCj5Ayy0c1CHl7H6bEuk6ikgtaXZrxkDe6NwMrpCu4jkzG
W5JfVvv8IozWssISTIhyDKq0qz+uDg==
=FE/1
-----END PGP SIGNATURE-----
)PGP";

/// A fixed Ed25519 device keypair, and the author's delegation of it.
/// Fixed rather than minted per run because the delegation signature
/// covers the device key, so a fresh key each time would need a fresh
/// signature each time -- which would mean the test signing its own
/// attestations, and proving nothing.
inline constexpr std::string_view kTestDevicePublicKeyHex =
    "535618716e531bea9985cb29e22e1ae86e3b78f61a4951e363b460d9ecad95c6";
inline constexpr std::string_view kTestDeviceSecretKeyHex =
    "d03cc92386a942ff91df41c6b7dc1c6a065357a4e9ddd7cd7677a78fd5c3d963"
    "7b72208256c3a8b5a37035b5f2884872416bd4eb0b7f4873ddc50171b841a7e4";

/// Signed over the delegation naming kTestDevicePublicKeyHex, device name
/// "peer-under-test", issued 1700000000.
inline constexpr std::string_view kDelegationForTestDeviceKey = R"PGP(
-----BEGIN PGP SIGNATURE-----

iQEzBAABCgAdFiEEIVGwSt+Z0KuYhszVq4PMLgodgPAFAmqYK/gACgkQq4PMLgod
gPCyHgf+J8fbT0OVf6bUp0uf0YJfBf6wc6bfQBtrVVNb+ev9HyEF38A9QaVlh8fc
l/EVWGxIpmSbv4GaRg4F70B5v7js1eIhpHz/9VtNymwjaDMKMbhXl5CK2gs00pux
aEyWOXNPpNXMzCTb5BIjWfdEBGnbDuPBQhf6yqQF9FD5cMy9EZVuIGlTHdCdGze0
OaM23mnW2ljykZjDEuJHX7uDkv2i/+aykEJHGaqeTDBfQPDn551NUsLDj92M/3v3
4medFfa+To+AiQd9udT1TJe/6GI9s2wvKCekd4w4GKe2+Aby3r2OqpNO+KvbW0/n
uAyesf9Z1vKqi3u/xC9ckuxQpRjVEQ==
=xM1/
-----END PGP SIGNATURE-----
)PGP";

} // namespace xudu::testing

#endif // XUDU_TESTS_PGP_FIXTURE_HPP
