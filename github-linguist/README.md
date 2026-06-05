# R# GitHub Linguist Submission Pack

GitHub will show `R#` in the repository language bar only after `R#` is added to the upstream `github-linguist` project.

Use this folder to prepare that PR.

## Files To Copy Into A github-linguist Fork

Copy this entry into:

```text
lib/linguist/languages.yml
```

From:

```text
github-linguist/languages.yml
```

Copy sample files into:

```text
samples/R#/
```

From:

```text
github-linguist/samples/R#/
```

## PR Checklist

- Add the `R#` language entry to `lib/linguist/languages.yml`.
- Add samples under `samples/R#/`.
- Run the Linguist test suite locally if possible.
- Open a pull request to `github/linguist`.
- After merge, wait for GitHub.com to deploy the new Linguist version.

Until that upstream merge/deploy happens, this repository cannot force GitHub to show `R#` in the Languages panel.
