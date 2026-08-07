---
name: ste
description: Write or revise technical and functional copy in ASD-STE100 Simplified Technical English (STE) — documentation, READMEs, code comments that instruct, and UI microcopy (error messages, tooltips, button labels, status text). Use when you write or review functional documentation, or when the user asks for STE, "simplified technical english", or clearer / plainer technical writing. Do NOT use for marketing, brand voice, or deliberately artful prose.
---

# Simplified Technical English (ASD-STE100)

> **Portable single-file edition.** This merges the rules, the word list, and the
> compliance checklist into one document. Use it two ways:
>
> - **As an agent skill** — save it as `.claude/skills/ste/SKILL.md` in a project
>   (or `~/.claude/skills/ste/SKILL.md` to make it global). The frontmatter above
>   is what makes it loadable; keep it.
> - **As plain documentation** — delete the frontmatter and drop the rest into a
>   style guide, a CONTRIBUTING file, or a project's agent instructions.

## Scope in this project (Phantom)

**In scope:** the root `README.md`, `tools/README.md`, `design/README.md`, and the
prose in `tools/*.py` — docstrings, comments, status lines, error messages, the
About dialog, and the command line help.

**Out of scope, permanently:** every word of the game's own writing. Pilot
dialogue, ship names and taglines, the backstory crawl, menu copy, any string
the player sees, and `DESIGN.md` (untracked, local only). The author keeps full
creative control there and that writing is not meant to be technically calibrated.
Comments in `src/` are engineering prose and are also out of scope.

The root `README.md` was out of scope and is now IN scope. See CLAUDE.md for why it
moved. Ship names and taglines quoted inside it are still the game's writing and stay
verbatim.

See `CLAUDE.md` at the repository root. If a request is ambiguous about which
side of that line it falls on, ask before you rewrite anything.

---

STE is a controlled language for technical documentation. It makes text easy to read, easy to translate, and hard to misread. It has two parts:

1. **Writing rules** — 53 rules in 9 sections (words, noun phrases, verbs, sentences, procedures, descriptions, warnings, punctuation, writing practices).
2. **A controlled dictionary** — about 900 approved words. Each approved word has one meaning and one part of speech. About 1200 more words are not approved and have approved alternatives.

This document does not contain the copyrighted dictionary. Apply the rules below and the common substitutions in the word list. **This is an unofficial aid. It is not affiliated with or endorsed by ASD.** Get the free official specification at https://www.asd-ste100.org/ . The human writer approves the final text.

## When to use — and when not

Use STE for **functional copy**: instructions, reference docs, READMEs, API and CLI help, error messages, tooltips, labels, status text, and code comments that instruct.

Do **not** apply STE to **voice copy**: marketing, taglines, brand names, storytelling, or deliberately artful prose. STE would flatten it. If a project marks certain copy as "voice", leave it alone. When unsure which a piece is, ask.

---

## Step 1 — Classify the text

Decide the type first, because two rules change with it:

- **Procedure** — steps the reader does. Use the imperative. Max sentence length **20 words**.
- **Description** — facts the reader reads. Max sentence length **25 words**.

## Step 2 — Apply the rules

**Words**
- Use one word for one meaning. Do not use a word two ways (for example "close" the valve vs a "close" fit).
- Do not use synonyms for variety. Pick one term and repeat it. (You can use "make sure"; do not switch between "verify", "check", "confirm", "ensure".)
- Prefer the approved word. See the word list below.
- Keep articles (a, an, the). Do not drop them for terseness.
- Do not use jargon, idioms, slang, or figurative language. Say the literal thing.
- Be specific. Replace vague words (thing, stuff, handle, deal with) with the exact noun or verb.

**Noun phrases**
- Do not string more than 3 nouns together. Break a long noun cluster with a preposition or a relative clause ("the temperature-sensor cable" → "the cable of the temperature sensor").

**Verbs**
- Use only these forms: infinitive, imperative, simple present, simple past, simple future, and the past participle **as an adjective**.
- Do not use the present perfect, past perfect, or progressive. ("We have received" → "We received"; "is running" → "runs".)
- Avoid `-ing`. Use it only in an approved technical name or as an established modifier. Rewrite an `-ing` clause as a full clause.
- Use the active voice. Use the passive only when the doer is unknown or unimportant, and mostly in descriptions.

**Sentences**
- Keep sentences short: ≤20 words (procedure), ≤25 words (description). One idea per sentence.
- Do not omit words to save space. Keep the subject, the verb, and the object.
- Connect a condition to its action, and put the condition first: "If the light is on, close the valve."

**Procedures**
- One instruction per sentence. Start the sentence with the verb.
- Put steps in order. Use a numbered or vertical list for more than ~3 steps.

**Descriptions**
- One topic per paragraph. Max ~6 sentences per paragraph.
- Use a list or a table for anything with more than ~3 parts.

**Warnings and cautions**
- Put a warning or caution **before** the step it applies to, not after.
- State the condition, then the consequence, in plain words.

**Punctuation**
- Prefer simple punctuation. Do not use a dash as a dramatic pause; split the sentence or use a comma or a colon.
- Use parentheses for a short aside only.

## Step 3 — Self-check

Read the result and confirm each item. For a full pass, use the checklist at the end.

- [ ] Every sentence is within the length limit and states one idea.
- [ ] Instructions use the imperative and start with the verb.
- [ ] Active voice, simple tenses, no `-ing` clauses.
- [ ] One term per concept, repeated — no synonyms, no double-meaning words.
- [ ] Articles present; no words omitted.
- [ ] No jargon, idioms, or figurative language; specific nouns and verbs.
- [ ] Warnings come before their step.

## Before / after

| Not STE | STE |
|---|---|
| The extension should be reloaded before testing. | Reload the extension before you test. |
| It couldn't read this image. | The extension could not read this image. |
| The record doesn't match — tampered data, or the wrong record. | The record does not match this image. The data changed, or this is the wrong record. |
| Utilize the config file to initiate the process. | Use the config file to start the process. |
| We have received the reports and are now processing them. | We received the reports. The extension processes them now. |
| The core has no opinion about what a record should contain. | The core does not define what a record contains. |
| A newsroom can record a photographer. (`record` is also a noun here) | A newsroom's record can hold a photographer. |

The last two are the failures that hide longest: **personification** ("the core has no opinion", "the parser does not care") reads as normal English but says nothing checkable, and **one word doing two jobs** ("a record" the noun vs "to record" the verb) passes every spellcheck while quietly breaking the one-word-one-meaning rule.

---

# Word list — common substitutions

This is a **curated, illustrative** list of common unapproved → approved substitutions. It is not the official ASD-STE100 dictionary, which is copyrighted and has about 900 approved and 1200 unapproved words. When a word is not in this list, prefer the shortest, most common, single-meaning word.

## Verbs and actions

| Do not use | Use |
|---|---|
| begin, commence, initiate, originate | start |
| terminate, cease, discontinue | stop |
| utilize, employ (a tool), leverage | use |
| assist, aid | help |
| require, necessitate | need |
| obtain, acquire | get |
| provide, supply, furnish | give |
| perform, execute, carry out, conduct | do |
| verify, check, confirm, ensure | make sure |
| examine, inspect (as "look at") | look at |
| indicate | show |
| permit | let / allow |
| retain | keep |
| eliminate, delete (physical) | remove |
| repair | fix / repair (keep one) |
| depress (a button) | push / press (keep one) |
| adhere to, comply with, follow (rules) | obey |
| attempt | try |
| ascertain, determine | find / find out |
| modify, alter | change |
| transmit, dispatch | send |
| illuminate | come on / light |

## Conditions, time, and connectors

| Do not use | Use |
|---|---|
| prior to | before |
| subsequent to, following | after |
| in the event of, in the event that | if |
| in order to | to |
| in the case of | for / if |
| due to the fact that, owing to | because |
| in conjunction with | with |
| additionally, furthermore, moreover | also |
| however | but |
| therefore, thus, hence | so |
| approximately | about |
| in the vicinity of | near |
| a number of, several | some / a few (be specific) |
| the majority of | most |

## Descriptions and quantities

| Do not use | Use |
|---|---|
| sufficient | enough |
| numerous, multiple | many |
| endeavor | try |
| facilitate | make easy / help |
| functionality | function / feature |
| implement (verb, in prose) | do / make / build (be specific) |
| optimal | best |
| prior | earlier |
| via | by / through / on |
| whilst | while |

## Vague words to replace with a specific noun or verb

`thing`, `stuff`, `handle`, `deal with`, `process` (as a vague verb), `manage`, `perform`, `content`, `data` (when a specific noun fits), `functionality`, `solution`, `leverage`.

Replace each with the exact noun or verb. For example: "handle the error" → "show the error message" or "stop and log the error".

## Punctuation habits to drop in functional copy

- A dash used as a dramatic pause. Split the sentence, or use a comma or a colon.
- The em dash for an aside. Use parentheses for a short aside, or a separate sentence.
- Long parenthetical chains. Break them into short sentences.

---

# Compliance checklist

Use this for a review pass. Go through each item for the text you write or revise. Fix a failure, then re-read.

## Text type
- [ ] I know whether this is a **procedure** (≤20 words/sentence) or a **description** (≤25 words/sentence).

## Words
- [ ] Each concept has ONE term. The same thing gets the same word every time. No synonyms for variety.
- [ ] No word carries two meanings or two parts of speech in this text.
- [ ] I used the approved / shortest common word.
- [ ] No jargon, idioms, slang, or figurative language.
- [ ] No vague words (thing, stuff, handle, deal with). Specific nouns and verbs instead.
- [ ] Articles (a, an, the) are present. No words were dropped to save space.

## Noun phrases
- [ ] No noun cluster has more than 3 nouns.

## Verbs
- [ ] Only these forms: infinitive, imperative, simple present, simple past, simple future, past participle as an adjective.
- [ ] No present perfect ("have done"), no past perfect, no progressive ("is doing").
- [ ] No `-ing` clause. `-ing` appears only in an approved name or an established modifier.
- [ ] Active voice. Passive only when the doer is unknown or unimportant.

## Sentences
- [ ] Every sentence is within the length limit.
- [ ] One idea per sentence.
- [ ] A condition comes before its action. ("If X, do Y.")

## Procedures
- [ ] One instruction per sentence.
- [ ] Each instruction starts with the verb.
- [ ] Steps are in order. A list is used for more than ~3 steps.

## Descriptions
- [ ] One topic per paragraph. No more than ~6 sentences per paragraph.
- [ ] A list or table is used for more than ~3 parts.

## Warnings and cautions
- [ ] A warning or caution comes BEFORE the step it applies to.
- [ ] It states the condition, then the consequence, in plain words.

## Punctuation
- [ ] No dash used as a dramatic pause. Sentences are split, or a comma/colon is used.
- [ ] Parentheses hold only a short aside.

## Final
- [ ] A non-expert reader can act on this text without help.
- [ ] A human writer has approved the result. No tool guarantees STE compliance.
