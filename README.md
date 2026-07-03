# Aether

Extended Mahimahi network emulator with cellular uplink buffering simulation.

## Overview

This repository contains:
- **Simulation_Tools**: New architecture simulating cellular driver-modem interaction
- **Trace Dataset**: Uplink throughput traces from US, Europe, and China

## Trace Dataset

Located in `Trace Dataset/`:

| Region | Format |
|--------|--------|
| `US/` | Mahimahi trace |
| `Europe/` | Mahimahi trace |
| `China/` | Mahimahi trace |

## Reference
```
@inproceedings{Aether,
    author = {Mu Wang, Yiying Lin, Shenghui Wei, Enhuan Dong, Kang Chen, Tong Li, Yinchao Zhang, Renjie Xie, Su Yao, Ke Xu, Changqiao Xu},
    title = {Forewarned is Forearmed: A Responsive Congestion Control with Non-intrusive Uplink Dynamics Capture},
    year = {2026},
    url = {https://doi.org/10.1145/3789240.3829143},
    booktitle={Proceedings of the ACM SIGCOMM 2026 Conference},
    series = {SIGCOMM '26}
}
```