# 【组员一负责】外部 AI 适配器自动测试
# 测试范围：合法候选点、坐标解析、网络转发、非法落点重试和本地兜底。
from __future__ import annotations

import json
import sys
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from examples.ai_server import (  # noqa: E402
    InvalidModelMove,
    Provider,
    call_upstream,
    choose_demo_move,
    extract_move,
    legal_candidate_moves,
)


def sample_payload() -> dict:
    board = [[0 for _ in range(15)] for _ in range(15)]
    board[7][7] = 1
    return {
        "protocol": "gomoku-ai/v1",
        "boardSize": 15,
        "board": board,
        "currentPlayer": 2,
        "history": [{"row": 7, "col": 7, "player": 1}],
    }


class MockChatHandler(BaseHTTPRequestHandler):
    received_authorization = ""
    received_payload: dict = {}
    response_contents = ['{"row": 7, "col": 8}']
    call_count = 0

    def do_POST(self) -> None:  # noqa: N802
        length = int(self.headers.get("Content-Length", "0"))
        type(self).received_authorization = self.headers.get("Authorization", "")
        type(self).received_payload = json.loads(self.rfile.read(length))
        response_index = min(
            type(self).call_count,
            len(type(self).response_contents) - 1,
        )
        content = type(self).response_contents[response_index]
        type(self).call_count += 1
        body = json.dumps(
            {
                "choices": [
                    {
                        "message": {
                            "content": f"```json\n{content}\n```"
                        }
                    }
                ]
            }
        ).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt: str, *args: object) -> None:
        pass


class AiAdapterTests(unittest.TestCase):
    def test_empty_board_candidates_only_contains_center(self) -> None:
        board = [[0 for _ in range(15)] for _ in range(15)]
        self.assertEqual(legal_candidate_moves(board, 15), [(7, 7)])

    def test_candidates_are_nearby_in_bounds_and_empty(self) -> None:
        payload = sample_payload()
        candidates = legal_candidate_moves(payload["board"], 15)
        self.assertIn((7, 8), candidates)
        self.assertNotIn((7, 7), candidates)
        self.assertTrue(candidates)
        for row, col in candidates:
            self.assertTrue(0 <= row < 15 and 0 <= col < 15)
            self.assertEqual(payload["board"][row][col], 0)
            self.assertLessEqual(max(abs(row - 7), abs(col - 7)), 2)

    def test_demo_move_is_legal(self) -> None:
        payload = sample_payload()
        row, col = choose_demo_move(payload)
        self.assertEqual(payload["board"][row][col], 0)
        self.assertEqual((row, col), (6, 7))

    def test_openai_compatible_forwarding(self) -> None:
        MockChatHandler.call_count = 0
        MockChatHandler.response_contents = ['{"row": 7, "col": 8}']
        server = ThreadingHTTPServer(("127.0.0.1", 0), MockChatHandler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            provider = Provider(
                f"http://127.0.0.1:{server.server_port}/chat/completions",
                "mock-model",
            )
            row, col, used_fallback = call_upstream(
                sample_payload(),
                provider,
                "test-secret",
                "mock-model",
            )
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)

        self.assertEqual((row, col), (7, 8))
        self.assertFalse(used_fallback)
        self.assertEqual(
            MockChatHandler.received_authorization,
            "Bearer test-secret",
        )
        self.assertEqual(
            MockChatHandler.received_payload["model"],
            "mock-model",
        )
        self.assertEqual(
            MockChatHandler.received_payload["messages"][0]["role"],
            "system",
        )

    def test_invalid_move_is_retried_with_feedback(self) -> None:
        MockChatHandler.call_count = 0
        MockChatHandler.response_contents = [
            '{"row": 7, "col": 7}',
            '{"row": 7, "col": 8}',
        ]
        server = ThreadingHTTPServer(("127.0.0.1", 0), MockChatHandler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            provider = Provider(
                f"http://127.0.0.1:{server.server_port}/chat/completions",
                "mock-model",
            )
            row, col, used_fallback = call_upstream(
                sample_payload(),
                provider,
                "key",
                "mock-model",
            )
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)

        self.assertEqual((row, col), (7, 8))
        self.assertFalse(used_fallback)
        self.assertEqual(MockChatHandler.call_count, 2)
        self.assertIn(
            "落点非法",
            MockChatHandler.received_payload["messages"][-1]["content"],
        )

    def test_repeated_invalid_moves_use_legal_adapter_fallback(self) -> None:
        MockChatHandler.call_count = 0
        MockChatHandler.response_contents = [
            '{"row": 7, "col": 7}',
            '{"row": "7", "col": "7"}',
        ]
        server = ThreadingHTTPServer(("127.0.0.1", 0), MockChatHandler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            provider = Provider(
                f"http://127.0.0.1:{server.server_port}/chat/completions",
                "mock-model",
            )
            row, col, used_fallback = call_upstream(
                sample_payload(),
                provider,
                "key",
                "mock-model",
            )
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)

        self.assertEqual((row, col), (6, 7))
        self.assertTrue(used_fallback)

    def test_string_coordinates_and_trailing_comma_are_accepted(self) -> None:
        payload = sample_payload()
        move = extract_move(
            '```json\n{"row": "7", "col": "8",}\n```',
            payload["board"],
            15,
        )
        self.assertEqual(move, (7, 8))

    def test_occupied_model_move_is_rejected(self) -> None:
        payload = sample_payload()
        with self.assertRaises(InvalidModelMove):
            extract_move(
                {"row": 7, "col": 7},
                payload["board"],
                15,
            )


if __name__ == "__main__":
    unittest.main()
