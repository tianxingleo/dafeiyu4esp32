import unittest
from unittest.mock import MagicMock, Mock

from whalemate import Companion, DeepSeek


def completion(content):
    response = MagicMock()
    response.__enter__.return_value = response
    response.json.return_value = {"choices": [{"message": {"content": content}}]}
    return response


class VoiceReplyTests(unittest.TestCase):
    def setUp(self):
        self.provider = DeepSeek("test-key", "test-model", "stella")

    def test_blank_json_retries_as_text(self):
        self.provider.post = Mock(side_effect=[completion("   "), completion("本鱼在呢！")])
        self.assertEqual(self.provider.reply("你好", [], []), ("idle", "本鱼在呢！"))
        first, second = self.provider.post.call_args_list
        self.assertEqual(first.kwargs["json"]["response_format"], {"type": "json_object"})
        self.assertNotIn("response_format", second.kwargs["json"])

    def test_valid_json_preserves_mood(self):
        self.provider.post = Mock(return_value=completion('{"mood":"happy","reply":"你好呀！"}'))
        self.assertEqual(self.provider.reply("你好", [], []), ("happy", "你好呀！"))
        self.provider.post.assert_called_once()

    def test_empty_fallback_is_not_reported_as_a_connection_failure(self):
        self.provider.transcribe = Mock(return_value="你好")
        self.provider.post = Mock(side_effect=[completion("{}"), completion(" ")])
        memory = Mock()
        memory.snapshot.return_value = (0, [], [])
        mood, line, audio = Companion(self.provider, False, memory).converse(b"audio")
        self.assertEqual((mood, audio), ("sad", b""))
        self.assertIn("没生成可读回复", line)
        memory.remember.assert_not_called()
