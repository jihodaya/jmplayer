# 옛 음악 파일 포맷 분석 기록

> **작성**: JJOME (GitHub [jihodaya](https://github.com/jihodaya) · YouTube [@jjome_Plus](https://www.youtube.com/@jjome_Plus))
> **프로젝트**: [JMPlayer](https://github.com/jihodaya/jmplayer) — 옛 컴퓨터 음악 재생기
> **최초 정리**: 2026-09-28
> **라이선스**: [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/deed.ko)
> **언어**: 한국어 원문 · [English translation](en/README.md)

이 폴더는 JMPlayer를 만들면서 알아낸 옛 음악 파일 포맷의 구조를 기록한 것이다.
대부분 공식 문서가 남아 있지 않은 포맷이고, 원본 프로그램을 디컴파일하거나
원본 프로그램이 만든 결과물과 대조해서 알아냈다. 프로그램은 언젠가 낡지만
이 기록은 다른 사람이 같은 파일을 다시 읽을 수 있게 하려고 남긴다.

## 출처 표기와 인용

이 문서들은 **CC BY 4.0** 으로 공개한다. 누구나 복사·수정·재배포·상업적 이용을
할 수 있지만, **작성자와 출처를 반드시 밝혀야 한다.** 다음과 같이 적으면 된다.

```
출처: JJOME (jihodaya), "JMPlayer 포맷 분석 기록", 2026.
      https://github.com/jihodaya/jmplayer
```

코드로 옮길 때는 소스 주석에 위 한 줄을 남겨 주기를 바란다.

## 문서 목록

| 파일 | 포맷 | 원본 프로그램 |
|---|---|---|
| [gyb.md](gyb.md) | `.GYB` 가요방 노래방 | GAYOBANG.EXE (DOS) |
| [oka-okm-okw.md](oka-okm-okw.md) | `.OKA` `.OKM` `.OKW` 옥소리 노래방 | NORE45.EXE (DOS) |
| [opl-instrument.md](opl-instrument.md) | GYB·OKA 공통 악기 레코드, `.BNK` 뱅크, 내장 악기 | GAYOBANG / NORE45 |
| [nob.md](nob.md) | `.NOB` 옥소리 노래방 (4.0 이전) | 옥소리 노래방 |
| [iss.md](iss.md) | `.ISS` IMS 가사 파일 | 한울소리 IMS |
| [rcp.md](rcp.md) | `.RCP` Recomposer + `.GSD` 셋업 | Recomposer (PC-98 / X68000) |
| [sng-ballade.md](sng-ballade.md) | `.SNG` Ballade | Ballade / ミュージくん / ミュージ郎 (PC-98) |
| [mdx.md](mdx.md) | `.MDX` + `.PDX` | MXDRV (X68000) |
| [mdz-mld.md](mdz-mld.md) | `.MDZ` MLD | MLD (X68000) |

## 표시 규칙

각 항목이 얼마나 확실한지 표시해 두었다.

| 표시 | 뜻 |
|---|---|
| ✅ | **확인됨** — 원본 프로그램의 코드, 원본이 만든 결과물, 또는 실기 녹음과 대조해서 맞는 것을 확인 |
| 📏 | **실측으로 맞춤** — 원리는 모르지만 많은 파일에서 측정해 값을 정함. 더 좋은 근거가 나오면 바뀔 수 있음 |
| ❓ | **미확인** — 아직 모름. 추측을 사실처럼 쓰지 않으려고 따로 표시 |

바이트 순서는 따로 말이 없으면 **리틀 엔디언**(x86 방식)이다. X68000 포맷(MDX,
MDZ)은 **빅 엔디언**(68000 방식)이다. 오프셋은 16진수로 쓴다.

## 어떻게 알아냈나 — 기준 답안

추측만으로는 틀리기 쉽다. 이 기록의 대부분은 **원본이 내놓은 답과 대조**해서
정했다. 각 포맷에서 쓴 기준 답안은 다음과 같다. 같은 작업을 다시 하는 사람에게
가장 쓸모 있는 부분이라고 생각한다.

| 포맷 | 기준 답안 | 방법 |
|---|---|---|
| GYB / OKA | GAYOBANG.EXE · NORE45.EXE 원본 | Ghidra로 디컴파일해서 OPL 드라이버를 읽음. DOSBox에서 원본으로 녹음한 소리와 우리 렌더링을 옥타브 대역별로 비교 |
| NOB | 라이브러리 574곡 | 가사 위치와 멜로디 채널 음표 시점을 맞춰 보는 회귀 분석 |
| RCP | 참조 `.MID`가 딸린 **303곡** | 변환 결과를 채널별·음표별로 비교 |
| SNG | **SNG2S 3.3** (M. Saito, 1993) | msdos-player로 64비트 Windows에서 실행. 파일 일부를 고쳐 넣고 결과를 보는 방식으로 필드를 알아냄 |
| MDX | **mxwav** (MXDRVg 동봉) | 원본 드라이버로 렌더링한 결과와 101곡 대조. 한두 음짜리 MDX를 직접 만들어 넣어 보는 방식이 가장 효과적이었음 |
| MDZ | **mlc.x · mdz2mus.x** (MLD247) | run68x로 Windows에서 실행. MML을 컴파일해서 나온 바이트를 읽고, 컴파일러가 알려 주는 트랙별 틱 수와 대조 |

한 가지 교훈: **한 곡으로 맞춘 값은 믿지 말 것.** 이 기록에서 틀렸다가 고친
항목의 대부분은 한두 곡에서는 맞아 보였지만 수백 곡에 돌려 보니 틀린 것이었다.
