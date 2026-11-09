"""Check causal attribution and non-destructive reviewed Git checkpoints."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from provenance_git import GitError, Repository
from provenance_store import Decisions, validate_intent_commit


class ProvenanceTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.repo = Repository(self.root)
        self.repo.git('init', '-b', 'main')
        self.repo.git('config', 'user.name', 'Test Author')
        self.repo.git('config', 'user.email', 'test@example.invalid')
        (self.root / '.gitignore').write_text('.cache/\n')
        self.store = Decisions(self.root)
        self.store.record({'id': 'decision-one', 'statement': 'Keep the human intent: λ.',
                           'origin': {'message': 'original-message'}})

    def artifact(self, path='code.cpp', text='int answer = 42;\n'):
        (self.root / path).write_text(text)

    def preview(self, paths):
        return self.store.preview('decision-one', paths,
                                  {'summary': 'Preserve a direct link to the reviewed change.',
                                   'author_role': 'agent', 'basis': 'explicit_record'})

    def initial(self):
        self.artifact()
        self.preview(['code.cpp', '.gitignore'])
        return self.store.checkpoint(next(iter(self.store.previews)))

    def test_frozen_diff_preserves_new_edits_and_unrelated_index(self):
        self.initial()
        self.artifact(text='int answer = 43;\n')
        self.artifact('unrelated.cpp', 'int staged = 7;\n')
        self.repo.git('add', 'unrelated.cpp')
        staged = self.repo.git('show', ':unrelated.cpp')
        preview = self.preview(['code.cpp'])
        self.artifact(text='int answer = 44;\n')
        result = self.store.checkpoint(preview['token'])
        self.assertEqual(self.repo.git('show', 'HEAD:code.cpp'), 'int answer = 43;\n')
        self.assertEqual((self.root / 'code.cpp').read_text(), 'int answer = 44;\n')
        self.assertEqual(self.repo.git('show', ':unrelated.cpp'), staged)
        self.assertNotIn('unrelated.cpp', self.repo.git('ls-tree', '--name-only', 'HEAD'))
        self.assertEqual(result['decisions'][0]['statement'], 'Keep the human intent: λ.')
        self.assertEqual(self.store.inspect_line('code.cpp', 1)['status'], 'uncommitted')
        self.assertEqual(self.store.inspect_line('code.cpp', 1)['decisions'], [])

    def test_head_race_and_index_lock_reject_without_modification(self):
        self.initial()
        self.artifact(text='int changed;\n')
        preview = self.preview(['code.cpp'])
        lock = self.root / '.git/index.lock'
        lock.touch()
        with self.assertRaisesRegex(GitError, 'owns the index'):
            self.store.checkpoint(preview['token'])
        lock.unlink()
        self.repo.git('commit', '--allow-empty', '-m', 'Another commit')
        head = self.repo.head()
        with self.assertRaisesRegex(GitError, 'HEAD moved'):
            self.store.checkpoint(preview['token'])
        self.assertEqual(self.repo.head(), head)

    def test_exact_statement_immutable_and_metadata_only_is_valuable(self):
        with self.assertRaisesRegex(GitError, 'immutable'):
            self.store.record({'id': 'decision-one', 'statement': 'Different words'})
        preview = self.store.preview('decision-one', [])
        self.store.checkpoint(preview['token'])
        self.assertEqual(self.repo.git('ls-tree', '--name-only', 'HEAD').strip(), '.canvas')
        self.artifact()
        with self.assertRaisesRegex(GitError, 'explicit rationale'):
            self.store.preview('decision-one', ['code.cpp'])

    def test_blame_and_unattributed_history(self):
        result = self.initial()
        evidence = self.store.inspect_line('code.cpp', 1)
        self.assertEqual(evidence['commit'], result['commit'])
        self.assertEqual(evidence['decisions'][0]['id'], 'decision-one')
        self.assertIn('+int answer', evidence['patch'])
        self.artifact(text='int unrelated_change;\n')
        self.repo.git('add', 'code.cpp')
        self.repo.git('commit', '-m', 'No intent recorded')
        self.assertEqual(self.store.inspect_line('code.cpp', 1)['decisions'], [])
        self.artifact('new.cpp')
        self.assertEqual(self.store.inspect_line('new.cpp', 1)['status'], 'untracked')
        self.repo.git('add', 'new.cpp')
        self.assertEqual(self.store.inspect_line('new.cpp', 1)['status'], 'uncommitted')

    def test_commit_hook_checks_durable_intent_scope_and_rationale(self):
        self.initial()
        self.artifact(text='int change;\n')
        self.repo.git('add', 'code.cpp')
        with self.assertRaisesRegex(GitError, 'Intent-ID'):
            validate_intent_commit(self.repo, 'Missing trailer')
        with self.assertRaisesRegex(GitError, 'must exist'):
            validate_intent_commit(self.repo, 'Intent-ID: missing\n')
        self.assertTrue(validate_intent_commit(self.repo, 'Intent-ID: decision-one\n'))
        self.artifact('unbound.cpp')
        self.repo.git('add', 'unbound.cpp')
        with self.assertRaisesRegex(GitError, 'covers artifact'):
            validate_intent_commit(self.repo, 'Intent-ID: decision-one\n')

    def test_real_commit_hook_rejects_unattributed_commit(self):
        self.initial()
        project = Path(__file__).resolve().parents[1]
        import shutil
        (self.root / '.githooks').mkdir()
        (self.root / 'tools').mkdir()
        shutil.copy2(project / '.githooks/commit-msg', self.root / '.githooks/commit-msg')
        for name in ['provenance_git.py', 'provenance_store.py']:
            shutil.copy2(project / 'tools' / name, self.root / 'tools' / name)
        self.repo.git('config', 'core.hooksPath', '.githooks')
        self.artifact(text='int hooked_change;\n')
        self.repo.git('add', 'code.cpp')
        head = self.repo.head()
        result = subprocess.run(['git', '-C', str(self.root), 'commit', '-m', 'Missing intent'],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Intent-ID', result.stderr)
        self.assertEqual(self.repo.head(), head)
        self.repo.git('commit', '-m', 'Reviewed change\n\nIntent-ID: decision-one\n')
        self.assertNotEqual(self.repo.head(), head)

    def test_evidence_uses_committed_rationale_not_later_edits(self):
        self.initial()
        self.store.set_rationale('decision-one', {'author_role': 'human', 'summary': 'Later note'})
        evidence = self.store.inspect_line('code.cpp', 1)
        self.assertNotEqual(evidence['decisions'][0]['rationale']['summary'], 'Later note')
        document = self.store.load()
        document['decisions'][0]['statement'] = 'Rewritten human words'
        self.store.save(document)
        self.repo.git('add', '.canvas/decisions.json')
        with self.assertRaisesRegex(GitError, 'immutable'):
            validate_intent_commit(self.repo, 'Intent-ID: decision-one\n')

    def test_rename_keeps_original_evidence(self):
        self.initial()
        self.repo.git('mv', 'code.cpp', 'renamed.cpp')
        self.repo.git('commit', '-m', 'Rename source')
        evidence = self.store.inspect_line('renamed.cpp', 1)
        self.assertIn('+int answer', evidence['patch'])
        self.assertEqual(evidence['decisions'][0]['id'], 'decision-one')


if __name__ == '__main__':
    unittest.main()
