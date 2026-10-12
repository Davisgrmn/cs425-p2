# Measured P2 transfer results

Input: 1,048,576 random bytes from `os.urandom`; 3 trials per configuration. Relay delay: 50 ms each direction. Timeout: 250 ms. Parallel runs: 12.

Every output file was compared byte for byte with the input. Sender elapsed time excludes the receiver's post-FIN linger. Raw trials are in `raw.csv`; commands and sender/receiver/relay logs are retained per run.

Relay: **instructor** (`/home/davis/cs425/cs425-p2/cs425_relay.py`). Its SHA-256 is recorded in `environment.json`.

| Window | Loss | Mean sender seconds | Std. dev. | KiB/s |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 0% | 104.553 | 0.006 | 9.794 |
| 16 | 0% | 6.660 | 0.000 | 153.754 |
| 1 | 5% | 133.007 | 1.168 | 7.699 |
| 16 | 5% | 26.193 | 0.700 | 39.094 |

Platform: `Linux-6.6.87.2-microsoft-standard-WSL2-x86_64-with-glibc2.39`.
Input SHA-256: `5c2ea501f3665657bbd75d6c184119209bbd270ae143beebc2baf0f2ad8b007b`.

GNU `/usr/bin/time` provides elapsed/user/system time and peak RSS when installed. The script otherwise records sender wall time using Python's monotonic performance clock.
