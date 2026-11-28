# Native encrypted 7z/RAR fixtures

The 7z fixtures are original MIT synthetic test data. `data.7z` and
`headers.7z` contain alpha.txt and sub/blå.txt, created in Haiku with
p7zip 17.05, LC_ALL=en_US.UTF-8, password `Listi-test-only`.
`headers.7z` additionally encrypts names. `links.7z` was created with
7-Zip 25.01 on Linux using -snl, storing a link to alpha.txt. Haiku's
p7zip command incorrectly records that synthetic link as a regular file,
so its output is not used as the symlink oracle.

`times.7z` was created with 7-Zip 25.01 on Linux (Store). Its three one-byte
files have UTC modification times of Unix epoch minus one second, epoch,
and 1700000000.125 seconds.

RAR fixtures are unmodified public libarchive test fixtures from
https://github.com/libarchive/libarchive/tree/master/libarchive/test :

- rar4-data.rar: test_read_format_rar_encryption_data.rar
- rar4-headers.rar: test_read_format_rar_encryption_header.rar
- rar5-mixed.rar: test_read_format_rar5_encrypted.rar
- rar5-headers.rar: test_read_format_rar5_encrypted_filenames.rar

RAR4 password: `12345678`. RAR5 headers password: `password`.
RAR5 mixed has unencrypted a/c, b uses `password`, d uses `password2`.
See LICENSE-libarchive.txt for upstream notices. No real user data or secrets.
