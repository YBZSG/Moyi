# 外部 AI Python 适配服务
# 文件职责：接收 gomoku-ai/v1 棋盘请求，连接不同大模型服务，解析并校验
# AI 返回坐标，处理重试、非法落点纠正和合法候选点约束。
"""gomoku-ai/v1 通用大模型适配器。

Qt 客户端始终请求本服务；本服务把棋盘转换成聊天模型请求，再把模型回答
转换为 {"row": int, "col": int}。

示例：
  http://127.0.0.1:8000/v1/move?provider=search&depth=3
  http://127.0.0.1:8000/v1/move?provider=deepseek
  http://127.0.0.1:8000/v1/move?provider=qwen
  http://127.0.0.1:8000/v1/move?provider=moonshot
  http://127.0.0.1:8000/v1/move?provider=ollama&model=qwen3

API Key 可以由 Qt 的 Bearer Token 输入框传入，也可以放在环境变量
GOMOKU_AI_API_KEY 中。服务只监听 127.0.0.1。
"""

from __future__ import annotations

import json
import os
import re
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any
from urllib.parse import parse_qs, urlparse


HOST = "127.0.0.1"
PORT = 8000
# 推理模型可能先生成较长的内部分析，再在结尾给出最终 moveId。
# 给它足够的时间和输出额度，等完整回答返回后再提取最终落点。
UPSTREAM_TIMEOUT_SECONDS = 240
MODEL_MAX_TOKENS = 4096


@dataclass(frozen=True)
class Provider:
    api_url: str
    default_model: str
    protocol: str = "openai-compatible"
    requires_key: bool = True


PROVIDERS: dict[str, Provider] = {
    "deepseek": Provider(
        "https://api.deepseek.com/chat/completions",
        "deepseek-chat",
    ),
    "qwen": Provider(
        "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions",
        "qwen-plus",
    ),
    "moonshot": Provider(
        "https://api.moonshot.cn/v1/chat/completions",
        "moonshot-v1-8k",
    ),
    "zhipu": Provider(
        "https://open.bigmodel.cn/api/paas/v4/chat/completions",
        "glm-4-flash",
    ),
    "ollama": Provider(
        "http://127.0.0.1:11434/api/chat",
        "qwen3",
        protocol="ollama",
        requires_key=False,
    ),
}


class UpstreamError(RuntimeError):
    """上游模型服务调用失败。"""


class InvalidModelMove(ValueError):
    """模型没有返回合法空位。"""


def validate_board(payload: dict[str, Any]) -> tuple[list[list[int]], int, int]:
    board = payload.get("board")
    size = int(payload.get("boardSize", 15))
    current_player = int(payload.get("currentPlayer", 2))

    if size < 5 or size > 25:
        raise ValueError("boardSize 超出支持范围")
    if not isinstance(board, list) or len(board) != size:
        raise ValueError(f"board 必须包含 {size} 行")
    if current_player not in (1, 2):
        raise ValueError("currentPlayer 必须是 1 或 2")

    normalized: list[list[int]] = []
    for row in board:
        if not isinstance(row, list) or len(row) != size:
            raise ValueError(f"board 每行必须包含 {size} 列")
        normalized_row = [int(cell) for cell in row]
        if any(cell not in (0, 1, 2) for cell in normalized_row):
            raise ValueError("棋盘单元只能是 0、1、2")
        normalized.append(normalized_row)
    return normalized, size, current_player


def choose_demo_move(payload: dict[str, Any]) -> tuple[int, int]:
    """无外部模型时的协议演示策略：选择最靠近中心的空位。"""
    board, size, _ = validate_board(payload)
    center = size // 2
    empty = [
        (row, col)
        for row in range(size)
        for col in range(size)
        if board[row][col] == 0
    ]
    if not empty:
        raise ValueError("棋盘已满")
    return min(
        empty,
        key=lambda point: (
            abs(point[0] - center) + abs(point[1] - center),
            point[0],
            point[1],
        ),
    )


def legal_candidate_moves(
    board: list[list[int]],
    size: int,
    limit: int = 96,
) -> list[tuple[int, int]]:
    """Return stable nearby legal moves that a language model can copy."""
    occupied = [
        (row, col)
        for row in range(size)
        for col in range(size)
        if board[row][col] != 0
    ]
    center = size // 2
    if not occupied:
        return [(center, center)]

    candidates: set[tuple[int, int]] = set()
    for stone_row, stone_col in occupied:
        for row in range(max(0, stone_row - 2), min(size, stone_row + 3)):
            for col in range(max(0, stone_col - 2), min(size, stone_col + 3)):
                if board[row][col] == 0:
                    candidates.add((row, col))

    return sorted(
        candidates,
        key=lambda point: (
            min(
                max(abs(point[0] - row), abs(point[1] - col))
                for row, col in occupied
            ),
            abs(point[0] - center) + abs(point[1] - center),
            point[0],
            point[1],
        ),
    )[:limit]


SEARCH_WIN_SCORE = 100_000_000
SEARCH_DIRECTIONS = ((0, 1), (1, 0), (1, 1), (1, -1))


class SearchTimeout(RuntimeError):
    """搜索达到本步时间限制。"""


def _inside(size: int, row: int, col: int) -> bool:
    return 0 <= row < size and 0 <= col < size


def _has_five(
    board: list[list[int]],
    size: int,
    row: int,
    col: int,
    player: int,
) -> bool:
    if not _inside(size, row, col) or board[row][col] != player:
        return False
    for delta_row, delta_col in SEARCH_DIRECTIONS:
        count = 1
        for sign in (-1, 1):
            next_row = row + sign * delta_row
            next_col = col + sign * delta_col
            while (
                _inside(size, next_row, next_col)
                and board[next_row][next_col] == player
            ):
                count += 1
                next_row += sign * delta_row
                next_col += sign * delta_col
        if count >= 5:
            return True
    return False


def _would_win(
    board: list[list[int]],
    size: int,
    row: int,
    col: int,
    player: int,
) -> bool:
    if board[row][col] != 0:
        return False
    board[row][col] = player
    won = _has_five(board, size, row, col, player)
    board[row][col] = 0
    return won


def _line_shape_score(count: int, open_ends: int) -> int:
    if count >= 5:
        return SEARCH_WIN_SCORE
    if count == 4:
        return 2_000_000 if open_ends == 2 else 180_000 if open_ends == 1 else 0
    if count == 3:
        return 70_000 if open_ends == 2 else 7_000 if open_ends == 1 else 0
    if count == 2:
        return 4_000 if open_ends == 2 else 450 if open_ends == 1 else 0
    return 100 if open_ends == 2 else 20 if open_ends == 1 else 0


def _placed_move_score(
    board: list[list[int]],
    size: int,
    row: int,
    col: int,
    player: int,
) -> int:
    """评估把 player 放在指定空位后形成的连续棋形。"""
    if board[row][col] != 0:
        return -SEARCH_WIN_SCORE
    board[row][col] = player
    score = 0
    strong_lines = 0
    for delta_row, delta_col in SEARCH_DIRECTIONS:
        count = 1
        open_ends = 0
        for sign in (-1, 1):
            next_row = row + sign * delta_row
            next_col = col + sign * delta_col
            while (
                _inside(size, next_row, next_col)
                and board[next_row][next_col] == player
            ):
                count += 1
                next_row += sign * delta_row
                next_col += sign * delta_col
            if (
                _inside(size, next_row, next_col)
                and board[next_row][next_col] == 0
            ):
                open_ends += 1
        line_score = _line_shape_score(count, open_ends)
        score += line_score
        if (count >= 4 and open_ends >= 1) or (count == 3 and open_ends == 2):
            strong_lines += 1
    board[row][col] = 0
    if strong_lines >= 2:
        score += 500_000
    return score


def _board_score(
    board: list[list[int]],
    size: int,
    perspective: int,
) -> int:
    """按整盘已有连续棋形进行静态估值。"""
    totals = {1: 0, 2: 0}
    for row in range(size):
        for col in range(size):
            player = board[row][col]
            if player == 0:
                continue
            for delta_row, delta_col in SEARCH_DIRECTIONS:
                previous_row = row - delta_row
                previous_col = col - delta_col
                if (
                    _inside(size, previous_row, previous_col)
                    and board[previous_row][previous_col] == player
                ):
                    continue
                count = 0
                next_row, next_col = row, col
                while (
                    _inside(size, next_row, next_col)
                    and board[next_row][next_col] == player
                ):
                    count += 1
                    next_row += delta_row
                    next_col += delta_col
                open_ends = 0
                if (
                    _inside(size, previous_row, previous_col)
                    and board[previous_row][previous_col] == 0
                ):
                    open_ends += 1
                if (
                    _inside(size, next_row, next_col)
                    and board[next_row][next_col] == 0
                ):
                    open_ends += 1
                totals[player] += _line_shape_score(count, open_ends)
    opponent = 3 - perspective
    return totals[perspective] - totals[opponent] * 11 // 10


def _ordered_search_moves(
    board: list[list[int]],
    size: int,
    player: int,
    limit: int,
) -> list[tuple[int, int]]:
    candidates = legal_candidate_moves(board, size, limit=225)
    if not candidates:
        return []

    winning = [
        move
        for move in candidates
        if _would_win(board, size, move[0], move[1], player)
    ]
    if winning:
        return winning

    opponent = 3 - player
    opponent_wins = {
        move
        for move in candidates
        if _would_win(board, size, move[0], move[1], opponent)
    }
    center = size // 2
    scored: list[tuple[int, int, int]] = []
    for row, col in candidates:
        attack = _placed_move_score(board, size, row, col, player)
        defense = _placed_move_score(board, size, row, col, opponent)
        score = attack + defense * 12 // 10
        if (row, col) in opponent_wins:
            score += 20_000_000
        score -= abs(row - center) + abs(col - center)
        scored.append((score, row, col))
    scored.sort(reverse=True)
    return [(row, col) for _, row, col in scored[:limit]]


def choose_search_move(
    payload: dict[str, Any],
    depth: int = 3,
    time_limit: float = 2.5,
) -> tuple[int, int, int]:
    """使用迭代加深 Negamax 与 Alpha-Beta 剪枝计算落点。"""
    board, size, current_player = validate_board(payload)
    depth = max(1, min(int(depth), 5))
    deadline = time.monotonic() + max(0.2, min(float(time_limit), 8.0))
    root_moves = _ordered_search_moves(board, size, current_player, 16)
    if not root_moves:
        raise ValueError("棋盘已满")
    if len(root_moves) == 1:
        return root_moves[0][0], root_moves[0][1], 1

    best_move = root_moves[0]
    completed_depth = 0

    def negamax(
        player: int,
        remaining_depth: int,
        alpha: int,
        beta: int,
        last_move: tuple[int, int] | None,
    ) -> int:
        if time.monotonic() >= deadline:
            raise SearchTimeout
        opponent = 3 - player
        if last_move and _has_five(
            board, size, last_move[0], last_move[1], opponent
        ):
            return -SEARCH_WIN_SCORE - remaining_depth
        if remaining_depth == 0:
            return _board_score(board, size, player)

        width = 10 if remaining_depth >= 3 else 8
        moves = _ordered_search_moves(board, size, player, width)
        if not moves:
            return 0
        best = -SEARCH_WIN_SCORE * 2
        for row, col in moves:
            board[row][col] = player
            try:
                value = -negamax(
                    opponent,
                    remaining_depth - 1,
                    -beta,
                    -alpha,
                    (row, col),
                )
            finally:
                board[row][col] = 0
            best = max(best, value)
            alpha = max(alpha, value)
            if alpha >= beta:
                break
        return best

    for target_depth in range(1, depth + 1):
        try:
            iteration_best = best_move
            iteration_score = -SEARCH_WIN_SCORE * 2
            alpha = -SEARCH_WIN_SCORE * 2
            for row, col in root_moves:
                if time.monotonic() >= deadline:
                    raise SearchTimeout
                board[row][col] = current_player
                try:
                    score = -negamax(
                        3 - current_player,
                        target_depth - 1,
                        -SEARCH_WIN_SCORE * 2,
                        -alpha,
                        (row, col),
                    )
                finally:
                    board[row][col] = 0
                if score > iteration_score:
                    iteration_score = score
                    iteration_best = (row, col)
                alpha = max(alpha, score)
            best_move = iteration_best
            completed_depth = target_depth
            root_moves.remove(best_move)
            root_moves.insert(0, best_move)
        except SearchTimeout:
            break

    return best_move[0], best_move[1], max(completed_depth, 1)


def ranked_model_candidate_moves(
    board: list[list[int]],
    size: int,
    current_player: int,
    limit: int = 48,
) -> list[tuple[int, int]]:
    """为聊天模型生成兼顾进攻与防守的候选点。"""
    candidates = legal_candidate_moves(board, size, limit=225)
    if not candidates:
        return []

    winning = [
        move
        for move in candidates
        if _would_win(board, size, move[0], move[1], current_player)
    ]
    if winning:
        return winning

    opponent = 3 - current_player
    forced_blocks = [
        move
        for move in candidates
        if _would_win(board, size, move[0], move[1], opponent)
    ]
    if forced_blocks:
        return forced_blocks

    center = size // 2
    scored: list[tuple[int, int, int]] = []
    for row, col in candidates:
        attack = _placed_move_score(
            board, size, row, col, current_player
        )
        defense = _placed_move_score(board, size, row, col, opponent)
        combined = attack + defense * 14 // 10
        combined -= abs(row - center) + abs(col - center)
        scored.append((combined, row, col))
    scored.sort(reverse=True)
    return [(row, col) for _, row, col in scored[:limit]]


def raw_model_candidate_moves(
    board: list[list[int]],
    size: int,
) -> list[tuple[int, int]]:
    """纯模型模式：按行列顺序提供整个棋盘上的所有空位。"""
    return [
        (row, col)
        for row in range(size)
        for col in range(size)
        if board[row][col] == 0
    ]


def build_messages(
    payload: dict[str, Any],
    candidate_moves: list[tuple[int, int]] | None = None,
    decision_mode: str = "guarded",
) -> list[dict[str, str]]:
    board, size, current_player = validate_board(payload)
    guarded = decision_mode != "raw"
    if candidate_moves is not None:
        candidates = candidate_moves
    elif guarded:
        candidates = ranked_model_candidate_moves(
            board, size, current_player
        )
    else:
        candidates = raw_model_candidate_moves(board, size)
    opponent = 3 - current_player
    own_wins = {
        move
        for move in candidates
        if _would_win(board, size, move[0], move[1], current_player)
    }
    opponent_wins = {
        move
        for move in candidates
        if _would_win(board, size, move[0], move[1], opponent)
    }
    candidate_lines = []
    for index, (row, col) in enumerate(candidates):
        if not guarded:
            candidate_lines.append(
                f"M{index}: row={row}, col={col}"
            )
            continue
        attack = _placed_move_score(
            board, size, row, col, current_player
        )
        defense = _placed_move_score(board, size, row, col, opponent)
        if (row, col) in own_wins:
            label = "立即取胜，必须优先"
        elif (row, col) in opponent_wins:
            label = "阻止对手下一手获胜，必须防守"
        elif defense >= 70_000:
            label = "高防守威胁"
        elif attack >= 70_000:
            label = "高进攻威胁"
        else:
            label = "普通候选"
        candidate_lines.append(
            f"M{index}: row={row}, col={col}, "
            f"进攻分={attack}, 防守分={defense}, 标签={label}"
        )
    candidate_text = "\n".join(candidate_lines)
    column_header = "列号    " + " ".join(
        f"{col:02d}" for col in range(size)
    )
    board_rows = [
        f"行{row:02d}: " + "  ".join(str(cell) for cell in board[row])
        for row in range(size)
    ]
    board_text = "\n".join([column_header, *board_rows])
    color = "黑棋" if current_player == 1 else "白棋"
    opponent_color = "白棋" if current_player == 1 else "黑棋"
    role_statement = (
        f"你在这一手执{color}，你的棋子值只能是 {current_player}；"
        f"对手执{opponent_color}，对手棋子值是 {opponent}。"
        f"你必须为{color}选择落点，绝不能站在对手立场决策。"
    )
    output_contract = (
        '最高优先级输出规则：你的整个回答必须且只能是一个 JSON 对象，'
        '格式为 {"moveId":"M数字"}。不要输出思考过程、局面分析、解释、'
        'Markdown、row、col 或任何其他文字。即使你需要内部思考，也只能'
        "在最终回答中给出一个候选编号。"
    )
    history = payload.get("history", [])
    last_move_text = "无（当前为开局）"
    if isinstance(history, list) and history:
        last_move = history[-1]
        if isinstance(last_move, dict):
            last_player = last_move.get("player", opponent)
            last_color = "黑棋" if last_player == 1 else "白棋"
            last_move_text = (
                f"{last_color}（值 {last_player}）刚刚下在 "
                f"row={last_move.get('row')}, col={last_move.get('col')}。"
                f"现在轮到你执{color}应对，必须重新评估该落点产生的威胁。"
            )
    if guarded:
        system_prompt = (
            f"{output_contract}{role_statement}你是五子棋落子引擎。"
            "棋盘值 0=空位、1=黑棋、"
            "2=白棋；坐标从 0 "
            "开始，row 表示从上到下，col 表示从左到右。优先立即取胜，其次"
            "阻止对手立即取胜；如果候选标签包含“必须防守”，必须阻止对手"
            "下一步获胜，不允许继续自己的普通进攻；之后再考虑活四、冲四、"
            "活三、双重威胁和中心控制。防守分表示该位置对对手的潜在价值，"
            "不得忽略。候选着法已经过合法性检查，你必须只选择一个候选编号。"
        )
    else:
        system_prompt = (
            f"{output_contract}{role_statement}你是独立的五子棋 AI。"
            "棋盘值 0=空位、1=黑棋、"
            "2=白棋；坐标从 0 "
            "开始，row 表示从上到下，col 表示从左到右。请自行分析完整棋盘，"
            "自行判断进攻、防守、取胜点、威胁和后续变化。适配器没有提供任何"
            "棋形评分、排序建议或强制战术，最终决策完全由你作出。你必须从所有"
            "合法空位编号中选择一个。"
        )
    user_prompt = (
        f"棋盘大小：{size}x{size}\n"
        f"本手身份（再次确认）：{role_statement}\n"
        f"对手最后一步：{last_move_text}\n"
        f"决策模式：{'纯模型决策' if not guarded else '战术约束决策'}\n"
        f"目标：五子连珠，长连也算胜利。\n"
        f"完整棋盘（带行列编号）：\n{board_text}\n"
        "合法候选着法：\n"
        f"{candidate_text}\n"
        "现在立即选择一个候选编号。不要复述或分析棋盘，不要写选择理由。"
        '整个回答只能是：{"moveId":"M数字"}'
    )
    return [
        {"role": "system", "content": system_prompt},
        {"role": "user", "content": user_prompt},
    ]


def extract_candidate_move(
    content: Any,
    candidates: list[tuple[int, int]],
    board: list[list[int]],
    size: int,
) -> tuple[int, int]:
    """优先解析候选编号，同时兼容旧模型返回的 row/col。"""
    data: Any = content
    text = ""
    if not isinstance(data, dict):
        text = str(content).strip()
        text = re.sub(
            r"<think>.*?</think>",
            "",
            text,
            flags=re.IGNORECASE | re.DOTALL,
        ).strip()
        if text.startswith("```"):
            text = re.sub(r"^```(?:json)?\s*", "", text, flags=re.IGNORECASE)
            text = re.sub(r"\s*```$", "", text)
        text = re.sub(r",\s*([}\]])", r"\1", text)
        try:
            data = json.loads(text)
        except json.JSONDecodeError:
            data = None
            # 推理模型常在 JSON 前后附带说明。优先提取最后一个包含
            # moveId 的 JSON 对象，而不是把整段文字判为非法。
            for object_match in reversed(
                re.findall(r"\{[^{}]*\}", text, flags=re.DOTALL)
            ):
                try:
                    parsed_object = json.loads(
                        re.sub(r",\s*([}\]])", r"\1", object_match)
                    )
                except json.JSONDecodeError:
                    continue
                if isinstance(parsed_object, dict) and any(
                    key in parsed_object
                    for key in (
                        "moveId",
                        "move_id",
                        "candidateId",
                        "candidate",
                        "move",
                    )
                ):
                    data = parsed_object
                    break

    direct_move_id: Any = None
    if data is None:
        direct_match = re.search(
            r"(?:moveId|move_id|candidateId|candidate|move)"
            r"\s*[:=]\s*[\"']?M?(\d+)",
            text,
            flags=re.IGNORECASE,
        )
        if not direct_match:
            direct_match = re.fullmatch(
                r"[\"']?M?(\d+)[\"']?",
                text,
                flags=re.IGNORECASE,
            )
        if direct_match:
            direct_move_id = direct_match.group(1)
        else:
            # 复杂局面下模型常写成“综合判断，我选择 M12”。
            # 回答只来自模型输出，不包含提示词，因此取最后一个编号即可。
            mentioned_ids = re.findall(
                r"(?<![A-Za-z0-9])M\s*[-#:]?\s*(\d+)\b",
                text,
                flags=re.IGNORECASE,
            )
            if mentioned_ids:
                direct_move_id = mentioned_ids[-1]

    if isinstance(data, dict):
        if isinstance(data.get("move"), dict):
            data = data["move"]
        move_id = data.get(
            "moveId",
            data.get(
                "move_id",
                data.get(
                    "candidateId",
                    data.get("candidate", data.get("move")),
                ),
            ),
        )
    else:
        move_id = direct_move_id

    if move_id is not None:
        match = re.fullmatch(r"M?(\d+)", str(move_id).strip(),
                             flags=re.IGNORECASE)
        if not match:
            raise InvalidModelMove("模型返回的 moveId 格式无效")
        index = int(match.group(1))
        if not 0 <= index < len(candidates):
            raise InvalidModelMove(
                f"模型返回的 moveId M{index} 不在候选列表中"
            )
        row, col = candidates[index]
        if board[row][col] != 0:
            raise InvalidModelMove("候选落点在等待期间已被占用")
        return row, col

    # 兼容仍然返回 {"row":...,"col":...} 的模型与自定义服务。
    try:
        return extract_move(content, board, size)
    except InvalidModelMove as error:
        preview = re.sub(r"\s+", " ", str(content)).strip()
        preview = preview[:120] if preview else "<空响应>"
        raise InvalidModelMove(
            f"未识别到合法 moveId；模型原始输出：{preview}"
        ) from error


def extract_move(content: Any, board: list[list[int]], size: int) -> tuple[int, int]:
    if isinstance(content, dict):
        data = content
    else:
        text = str(content).strip()
        if text.startswith("```"):
            text = re.sub(r"^```(?:json)?\s*", "", text, flags=re.IGNORECASE)
            text = re.sub(r"\s*```$", "", text)
        text = re.sub(r",\s*([}\]])", r"\1", text)
        try:
            data = json.loads(text)
        except json.JSONDecodeError:
            match = re.search(r"\{.*?\}", text, flags=re.DOTALL)
            if match:
                try:
                    object_text = re.sub(
                        r",\s*([}\]])",
                        r"\1",
                        match.group(0),
                    )
                    data = json.loads(object_text)
                except json.JSONDecodeError as error:
                    raise InvalidModelMove("模型返回的落点 JSON 无效") from error
            else:
                coordinate_match = re.search(
                    r"row\s*[:=]\s*[\"']?(-?\d+)[\"']?.*?"
                    r"col\s*[:=]\s*[\"']?(-?\d+)",
                    text,
                    flags=re.IGNORECASE | re.DOTALL,
                )
                if not coordinate_match:
                    raise InvalidModelMove("模型没有返回 JSON 落点")
                data = {
                    "row": int(coordinate_match.group(1)),
                    "col": int(coordinate_match.group(2)),
                }

    if not isinstance(data, dict):
        raise InvalidModelMove("模型返回的落点必须是 JSON 对象")
    if isinstance(data.get("move"), dict):
        data = data["move"]

    def coordinate(value: Any) -> int | None:
        if isinstance(value, bool):
            return None
        if isinstance(value, int):
            return value
        if isinstance(value, float) and value.is_integer():
            return int(value)
        if isinstance(value, str) and re.fullmatch(r"-?\d+", value.strip()):
            return int(value.strip())
        return None

    row = coordinate(data.get("row"))
    col = coordinate(data.get("col"))
    if row is None or col is None:
        raise InvalidModelMove("模型返回的 row/col 必须是整数")
    if not (0 <= row < size and 0 <= col < size):
        raise InvalidModelMove(f"模型返回的落点 ({row}, {col}) 超出棋盘")
    if board[row][col] != 0:
        raise InvalidModelMove(f"模型返回的落点 ({row}, {col}) 已有棋子")
    return row, col


def request_chat_content(
    messages: list[dict[str, str]],
    provider: Provider,
    api_key: str,
    model: str,
) -> Any:
    if provider.protocol == "ollama":
        request_data = {
            "model": model,
            "messages": messages,
            "stream": False,
            "format": "json",
        }
    else:
        request_data = {
            "model": model,
            "messages": messages,
            "temperature": 0.1,
            # 推理模型可能先生成 reasoning_content；必须等它生成完最终 JSON，
            # 否则会在 moveId 出现前被截断并被误判为非法落点。
            "max_tokens": MODEL_MAX_TOKENS,
            "stream": False,
            "response_format": {"type": "json_object"},
        }

    headers = {
        "Content-Type": "application/json",
        "Accept": "application/json",
        "User-Agent": "Qt-Gomoku-AI-Adapter/1.0",
    }
    if api_key:
        headers["Authorization"] = f"Bearer {api_key}"

    response_data: dict[str, Any] | None = None
    transient_statuses = {408, 425, 429, 500, 502, 503, 504}
    for request_attempt in range(3):
        request = urllib.request.Request(
            provider.api_url,
            data=json.dumps(request_data, ensure_ascii=False).encode("utf-8"),
            headers=headers,
            method="POST",
        )
        try:
            with urllib.request.urlopen(
                request,
                timeout=UPSTREAM_TIMEOUT_SECONDS,
            ) as response:
                response_data = json.loads(response.read().decode("utf-8"))
            break
        except urllib.error.HTTPError as error:
            error_body = error.read().decode("utf-8", errors="replace")
            error_message = ""
            try:
                error_json = json.loads(error_body)
                error_value = error_json.get("error", "")
                if isinstance(error_value, dict):
                    error_message = str(error_value.get("message", ""))
                else:
                    error_message = str(error_value)
            except json.JSONDecodeError:
                error_message = error_body
            error_message = re.sub(r"\s+", " ", error_message).strip()[:180]
            unsupported_json_mode = (
                error.code == 400
                and "response_format" in request_data
                and any(
                    keyword in error_message.lower()
                    for keyword in (
                        "response_format",
                        "json mode",
                        "json_object",
                        "unsupported",
                    )
                )
            )
            if unsupported_json_mode:
                # 部分自定义 OpenAI 兼容服务不支持 JSON Mode；
                # 去掉该可选参数后仍使用严格提示词重试。
                request_data.pop("response_format", None)
                continue
            if error.code in transient_statuses and request_attempt < 2:
                time.sleep(0.8 * (request_attempt + 1))
                continue
            detail = f"：{error_message}" if error_message else ""
            raise UpstreamError(
                f"上游 AI 返回 HTTP {error.code}{detail}"
            ) from error
        except urllib.error.URLError as error:
            if request_attempt < 1:
                time.sleep(0.6)
                continue
            raise UpstreamError(f"无法连接上游 AI：{error.reason}") from error
        except (TimeoutError, json.JSONDecodeError) as error:
            if request_attempt < 1:
                time.sleep(0.6)
                continue
            raise UpstreamError("上游 AI 超时或返回了无效 JSON") from error

    if response_data is None:
        raise UpstreamError("上游 AI 没有返回数据")

    try:
        if provider.protocol == "ollama":
            message = response_data["message"]
            finish_reason = response_data.get("done_reason", "")
        else:
            choice = response_data["choices"][0]
            message = choice["message"]
            finish_reason = choice.get("finish_reason", "")

        content = message.get("content")
        if isinstance(content, list):
            content = "".join(
                str(item.get("text", "")) if isinstance(item, dict)
                else str(item)
                for item in content
            )
        if content not in (None, ""):
            return content

        # 一些推理模型把全部文本放在 reasoning_content，content 为空。
        reasoning_content = message.get("reasoning_content")
        if reasoning_content not in (None, ""):
            return reasoning_content
        raise UpstreamError(
            f"上游 AI 返回空内容（finish_reason={finish_reason or '未知'}）"
        )
    except (KeyError, IndexError, TypeError) as error:
        raise UpstreamError("上游 AI 响应中缺少 message.content") from error


def call_upstream(
    payload: dict[str, Any],
    provider: Provider,
    api_key: str,
    model: str,
    decision_mode: str = "guarded",
) -> tuple[int, int, str]:
    board, size, current_player = validate_board(payload)
    if decision_mode == "raw":
        candidate_moves = raw_model_candidate_moves(board, size)
    else:
        candidate_moves = ranked_model_candidate_moves(
            board,
            size,
            current_player,
        )
    candidate_ids = ", ".join(
        f"M{index}" for index in range(len(candidate_moves))
    )
    messages = build_messages(
        payload,
        candidate_moves,
        decision_mode=decision_mode,
    )
    last_error: InvalidModelMove | None = None

    # 大模型偶尔会选中已有棋子。第一次非法时把具体原因反馈给模型，
    # 再请求一次，而不是立即让整局切换到内置 AI。
    for attempt in range(2):
        content = request_chat_content(messages, provider, api_key, model)
        try:
            row, col = extract_candidate_move(
                content,
                candidate_moves,
                board,
                size,
            )
            return row, col, ""
        except InvalidModelMove as error:
            last_error = error
            if attempt == 1:
                break
            # 不把上一段冗长分析重新放进上下文，否则部分模型会继续解释。
            # 第二次请求只保留原局面，并追加最短的格式纠正指令。
            messages = [
                messages[0],
                messages[1],
                {
                    "role": "user",
                    "content": (
                        f"上一回答无法作为落点使用：{error}。停止分析，"
                        "不要解释，不要输出坐标。现在只返回一个 JSON 对象："
                        '{"moveId":"M数字"}。'
                        f"允许的编号只有：{candidate_ids}"
                    ),
                },
            ]

    # 上游已经真实调用，但连续给出非法落点。纯模型模式如实报告失败；
    # 战术约束模式才允许从已排序的合法候选中进行本地纠正。
    if decision_mode == "raw":
        raise InvalidModelMove(
            f"纯模型模式连续返回非法落点：{last_error}"
        )
    if not candidate_moves:
        raise InvalidModelMove("棋盘上没有合法候选点")
    fallback_row, fallback_col = candidate_moves[0]
    return fallback_row, fallback_col, str(last_error or "未知格式错误")


def resolve_provider(
    query: dict[str, list[str]],
) -> tuple[str, Provider | None, str]:
    provider_name = query.get(
        "provider",
        [os.getenv("GOMOKU_AI_PROVIDER", "demo")],
    )[0].strip().lower()

    if provider_name in ("", "demo", "example"):
        return "demo", None, "demo"
    if provider_name == "search":
        depth_text = query.get("depth", ["3"])[0].strip()
        try:
            depth = int(depth_text)
        except ValueError as error:
            raise ValueError("search 模式的 depth 必须是整数") from error
        if not 1 <= depth <= 5:
            raise ValueError("search 模式的 depth 必须在 1 至 5 之间")
        return "search", None, str(depth)
    if provider_name == "custom":
        api_url = os.getenv("GOMOKU_AI_URL", "").strip()
        default_model = os.getenv("GOMOKU_AI_MODEL", "").strip()
        protocol = os.getenv("GOMOKU_AI_PROTOCOL", "openai-compatible").strip()
        if not api_url or not default_model:
            raise ValueError(
                "custom 模式需要设置 GOMOKU_AI_URL 和 GOMOKU_AI_MODEL"
            )
        provider = Provider(
            api_url,
            default_model,
            protocol=protocol,
            requires_key=protocol != "ollama",
        )
    else:
        provider = PROVIDERS.get(provider_name)
        if provider is None:
            supported = ", ".join(
                ["demo", "search", *PROVIDERS.keys(), "custom"]
            )
            raise ValueError(f"未知 provider；支持：{supported}")

    model = query.get(
        "model",
        [os.getenv("GOMOKU_AI_MODEL", provider.default_model)],
    )[0].strip()
    if not model:
        raise ValueError("模型名称不能为空")
    return provider_name, provider, model


def bearer_token(headers: Any) -> str:
    authorization = headers.get("Authorization", "").strip()
    if authorization.lower().startswith("bearer "):
        return authorization[7:].strip()
    return os.getenv("GOMOKU_AI_API_KEY", "").strip()


class GomokuHandler(BaseHTTPRequestHandler):
    server_version = "GomokuAIAdapter/2.7"

    def do_GET(self) -> None:  # noqa: N802 - HTTP handler API
        parsed = urlparse(self.path)
        if parsed.path != "/health":
            self.send_error(404, "Not Found")
            return
        self._send_json(
            200,
            {
                "status": "ok",
                "protocol": "gomoku-ai/v1",
                "modelMoveFormat": "moveId",
                "modelTactics": "attack-defense-constrained",
                "cloudDecisionModes": ["guarded", "raw"],
                "providers": ["demo", "search", *PROVIDERS.keys(), "custom"],
            },
        )

    def do_POST(self) -> None:  # noqa: N802 - HTTP handler API
        parsed = urlparse(self.path)
        if parsed.path != "/v1/move":
            self.send_error(404, "Not Found")
            return

        try:
            length = int(self.headers.get("Content-Length", "0"))
            payload = json.loads(self.rfile.read(length))
            query = parse_qs(parsed.query)
            provider_name, provider, model = resolve_provider(query)
            decision_mode = query.get("mode", ["guarded"])[0].strip().lower()
            if decision_mode not in ("guarded", "raw"):
                raise ValueError("mode 只支持 guarded 或 raw")

            if provider_name == "demo":
                row, col = choose_demo_move(payload)
                detail = "Python 协议演示"
            elif provider_name == "search":
                depth = int(model)
                row, col, reached_depth = choose_search_move(
                    payload,
                    depth=depth,
                )
                detail = (
                    f"Python Alpha-Beta 搜索 AI"
                    f"（目标深度 {depth}，完成 {reached_depth}）"
                )
            else:
                assert provider is not None
                api_key = bearer_token(self.headers)
                if provider.requires_key and not api_key:
                    self._send_json(
                        401,
                        {"error": f"{provider_name} 模式缺少 API Key"},
                    )
                    return
                row, col, adapter_correction_reason = call_upstream(
                    payload,
                    provider,
                    api_key,
                    model,
                    decision_mode=decision_mode,
                )
                mode_name = "纯模型" if decision_mode == "raw" else "战术约束"
                detail = f"{provider_name}/{model}（{mode_name}）"
                if adapter_correction_reason:
                    detail += (
                        "（模型落点非法："
                        f"{adapter_correction_reason}；适配器已纠正）"
                    )

            self._send_json(
                200,
                {"row": row, "col": col, "message": detail},
            )
        except InvalidModelMove as error:
            self._send_json(502, {"error": str(error)})
        except UpstreamError as error:
            self._send_json(502, {"error": str(error)})
        except (KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
            self._send_json(400, {"error": str(error)})

    def log_message(self, fmt: str, *args: object) -> None:
        print(f"[gomoku-ai] {self.address_string()} - {fmt % args}")

    def _send_json(self, status: int, data: dict[str, Any]) -> None:
        body = json.dumps(data, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


if __name__ == "__main__":
    server = ThreadingHTTPServer((HOST, PORT), GomokuHandler)
    print(f"Gomoku AI adapter: http://{HOST}:{PORT}/v1/move")
    print("Health check:       http://127.0.0.1:8000/health")
    print("Press Ctrl+C to stop.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n服务已停止")
    finally:
        server.server_close()
