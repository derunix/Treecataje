#!/usr/bin/env python3
"""End-to-end MCP self-test: spawn mcp_server.py over stdio, list tools, and
call a few against the real device. Proves the full chain before Claude connects.
"""
import os
import asyncio

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

HERE = os.path.dirname(os.path.abspath(__file__))


async def main():
    params = StdioServerParameters(
        command=os.path.join(HERE, ".venv/bin/python"),
        args=[os.path.join(HERE, "mcp_server.py")],
        env={**os.environ, "COMPANION_PORT": "/dev/ttyACM1", "COMPANION_TOKEN": ""},
    )
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as s:
            await s.initialize()
            tools = await s.list_tools()
            print("tools:", [t.name for t in tools.tools])

            for name, args in [
                ("device_connect", {}),
                ("device_status", {}),
                ("device_run", {"command": "free"}),
                ("device_busy", {}),
            ]:
                res = await s.call_tool(name, args)
                text = res.content[0].text if res.content else "(empty)"
                print(f"\n=== {name}({args}) ===\n{text}")


if __name__ == "__main__":
    asyncio.run(main())
