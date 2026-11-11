import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import intents


def turn(identifier, **evidence):
    return {"id": identifier, "source": "transcript", "said": identifier, "why": "",
            "intent": identifier, "changes": {}, **evidence}


class IntentStoreTest(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.root = Path(self.folder.name)
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)

    def tearDown(self):
        self.folder.cleanup()

    def write(self, name, text):
        (self.root / name).write_text(text)
        subprocess.run(["git", "-C", str(self.root), "add", name], check=True)

    def owners(self, name, turns):
        store = {"version": 2, "intents": [], "commits": {}, "turns": turns}
        return intents.blame(self.root, store)[name]

    def test_reformatted_lines_keep_their_owner(self):
        self.write("a.py", "def  zoom( camera ):\n        return camera.zoom * 2\n")
        first = turn("zoom", changes={"a.py": intents.hashes(["def zoom(camera):",
                                                             "    return camera.zoom * 2"])})
        self.assertEqual(self.owners("a.py", [first]), [0, 0])

    def test_first_turn_to_write_a_line_owns_it_after_a_move(self):
        line = "const auto point = camera.world_to_screen(origin, viewport());"
        self.write("new.cpp", line + "\n")
        wrote = turn("wrote", changes={"old.cpp": intents.hashes([line])})
        moved = turn("moved", changes={"new.cpp": intents.hashes([line])})
        self.assertEqual(self.owners("new.cpp", [wrote, moved]), [1])
        self.write("elsewhere.cpp", line + "\n")
        self.assertEqual(self.owners("elsewhere.cpp", [wrote, moved]), [0])

    def test_transcript_edits_outrank_a_commit_that_swept_them_up(self):
        line = "renderer->draw(label.mesh, position, camera.zoom);"
        self.write("view.cpp", line + "\n")
        swept = turn("earlier", committed={"view.cpp": intents.hashes([line])})
        wrote = turn("later", changes={"view.cpp": intents.hashes([line])})
        self.assertEqual(self.owners("view.cpp", [swept, wrote]), [1])

    def test_braces_and_milestone_lines_follow_their_neighbours(self):
        known, vague = "int answer = compute_the_answer();", "int other = compute_another();"
        self.write("m.cpp", "void run()\n{\n" + known + "\n" + vague + "\n}\n\nint unrelated;\n")
        blob = turn("blob", milestone={"m.cpp": intents.hashes([vague, "void run()"])})
        exact = turn("exact", changes={"m.cpp": intents.hashes([known])})
        self.assertEqual(self.owners("m.cpp", [blob, exact]), [1, 1, 1, 1, 1, None, None])

    def test_blank_lines_inside_one_turn_do_not_split_it(self):
        first, second, other = "alpha_value = compute(1)", "beta_value = compute(2)", "gamma = 3333"
        self.write("b.py", first + "\n\n" + second + "\n\n" + other + "\n")
        one = turn("one", changes={"b.py": intents.hashes([first, second])})
        two = turn("two", changes={"b.py": intents.hashes([other])})
        self.assertEqual(self.owners("b.py", [one, two]), [0, 0, 0, None, 1])

    def test_recording_a_turn_claims_the_working_tree_changes(self):
        self.write("kept.py", "already_here = compute_everything()\n")
        subprocess.run(["git", "-C", str(self.root), "-c", "user.name=t", "-c", "user.email=t@t",
                        "commit", "-qm", "base"], check=True)
        (self.root / "kept.py").write_text(
            "already_here = compute_everything()\nadded_later = compute_more()\n")
        (self.root / "new.py").write_text("brand_new_value = compute_fresh()\n")
        intents.save(self.root, {"version": 2, "intents": [{"id": "grow", "statement": "Grow."}],
                                 "commits": {}, "turns": []})
        intents.record(self.root, "make it grow", "Added two values.", "grow", "HEAD", "t1")
        intents.record(self.root, "make it grow", "Added two values.", "grow", "HEAD", "t1")
        store = intents.load(self.root)
        self.assertEqual(len(store["turns"]), 1)
        owners = intents.blame(self.root, store)
        self.assertEqual(owners["kept.py"], [None, 0])
        self.assertNotIn("new.py", owners)  # untracked files appear once Git tracks them
        self.assertEqual(sorted(store["turns"][0]["changes"]), ["kept.py", "new.py"])

    def test_canvas_context_is_where_not_what_was_said(self):
        context = json.dumps({"path": "camera.h", "line": 42, "workspace": "/x"})
        said, where = intents.human_text([
            {"text": "<external_source-canvas>" + context + "</external_source-canvas>"},
            {"text": "<environment_context>ignored</environment_context>"},
            {"text": "why is this smooth?"}])
        self.assertEqual(said, "why is this smooth?")
        self.assertEqual(where, {"path": "camera.h", "line": 42})

    def test_view_gives_line_ranges_per_file(self):
        self.write("a.py", "first_line_of_code = 1\nsecond_line_of_code = 2\n\nthird = 3\n")
        store = {"version": 2, "intents": [{"id": "one", "statement": "One."}], "commits": {},
                 "turns": [turn("one", changes={"a.py": intents.hashes(
                     ["first_line_of_code = 1", "second_line_of_code = 2"])})]}
        intents.save(self.root, store)
        self.assertEqual(intents.load(self.root)["turns"][0]["changes"], store["turns"][0]["changes"])
        self.assertEqual(intents.view(self.root)["files"], {"a.py": [[1, 2, 0]]})


if __name__ == "__main__":
    unittest.main()
