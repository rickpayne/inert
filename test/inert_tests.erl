%%% Copyright (c) 2013, Michael Santos <michael.santos@gmail.com>
%%%
%%% Permission to use, copy, modify, and/or distribute this software for any
%%% purpose with or without fee is hereby granted, provided that the above
%%% copyright notice and this permission notice appear in all copies.
%%%
%%% THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
%%% WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
%%% MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
%%% ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
%%% WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
%%% ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
%%% OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
-module(inert_tests).

-compile(export_all).

-include_lib("eunit/include/eunit.hrl").

inert_test_() ->
    {setup,
        fun start/0,
        fun stop/1,
        fun run/1
    }.

run(Ref) ->
    [
        inert_badfd(Ref),
        inert_select(Ref),
        inert_stream(Ref),
        inert_poll_timeout(Ref),
        inert_stateless_fdset(Ref),
        inert_controlling_process(Ref),
        inert_error_closed()
    ].

start() ->
    {ok, Ref} = inert:start(),
    Ref.

stop(Ref) ->
    inert:stop(Ref).

inert_badfd(Ref) ->
    [
        ?_assertEqual({error, ebadfd}, inert:fdset(Ref, -1)),
        ?_assertEqual({error, ebadfd}, inert:poll(Ref, -1)),
        ?_assertEqual({error, ebadfd}, inert:fdset(Ref, 127)),
        ?_assertEqual({error, ebadfd}, inert:fdset(Ref, 128)),
        ?_assertEqual({error, ebadfd}, inert:fdset(Ref, 10000))
    ].

inert_select(Ref) ->
    {ok, Sock1} = gen_tcp:listen(0, [binary,{active,false}]),
    {ok, Sock2} = gen_tcp:listen(0, [binary,{active,false}]),

    {ok, Port1} = inet:port(Sock1),
    {ok, Port2} = inet:port(Sock2),

    {ok, FD1} = inet:getfd(Sock1),
    {ok, FD2} = inet:getfd(Sock2),

    ok = inert:fdset(Ref, FD1),
    ok = inert:fdset(Ref, FD2),

    {ok, C1} = gen_tcp:connect("localhost", Port1, []),
    {ok, C2} = gen_tcp:connect("localhost", Port2, []),

    gen_tcp:close(C1),
    gen_tcp:close(C2),

    Result = receive
        {inert_read, _, FD1} ->
%            error_logger:info_report([{fd, FD1}]),
            inert:fdclr(Ref, FD1),
            receive
                {inert_read, _, FD2} ->
%                    error_logger:info_report([{fd, FD2}]),
                    inert:fdclr(Ref, FD2)
            end
    end,
    ?_assertEqual(ok, Result).

inert_stream(Ref) ->
    N = getenv("INERT_TEST_STREAM_RUNS", 10),

    {ok, Socket} = gen_tcp:listen(0, [binary,{active,false}]),
    {ok, Port} = inet:port(Socket),
    spawn(fun() -> connect(Port, N) end),
    accept(Ref, Socket, N).

accept(Ref,S,N) ->
    accept(Ref,S,N,N).

accept(_Ref, S, X, 0) ->
    wait(S, X);
accept(Ref, S, X, N) ->
    {ok, S1} = gen_tcp:accept(S),
    {ok, FD} = inet:getfd(S1),
    Self = self(),
    spawn(fun() -> read(Ref, Self, FD) end),
    accept(Ref, S, X, N-1).

wait(S, 0) ->
    ?_assertEqual(ok, gen_tcp:close(S));
wait(S, N) ->
    receive
        {fd_close, _FD} ->
            wait(S, N-1)
    end.

read(Ref, Pid, FD) ->
    read(Ref, Pid, FD, 0).
read(Ref, Pid, FD, N) ->
    ok = inert:poll(Ref, FD),
    case procket:read(FD, 1) of
        {ok, <<>>} ->
            procket:close(FD),
%            error_logger:info_report([
%                    {fd, FD},
%                    {read_bytes, N}
%                ]),
            N = getenv("INERT_TEST_STREAM_NUM_BYTES", 1024),
            Pid ! {fd_close, FD};
        {ok, Buf} ->
            read(Ref, Pid, FD, N + byte_size(Buf));
        {error, eagain} ->
            error_logger:info_report([{fd, FD}, {error, eagain}]),
            read(Ref, Pid, FD, N);
        {error, Error} ->
            error_logger:error_report([{fd, FD}, {error, Error}])
    end.

connect(Port, N) ->
    {ok, C} = gen_tcp:connect("localhost", Port, []),
    Num = getenv("INERT_TEST_STREAM_NUM_BYTES", 1024),
    Bin = crypto:rand_bytes(Num),
    ok = gen_tcp:send(C, Bin),
    ok = gen_tcp:close(C),
    connect(Port, N-1).

inert_poll_timeout(Ref) ->
    {ok, Socket} = gen_tcp:listen(0, [binary, {active,false}]),
    {ok, FD} = inet:getfd(Socket),
    ?_assertEqual({error, timeout}, inert:poll(Ref, FD, [{timeout, 10}])).

% Test successive calls to fdset overwrite the previous mode
inert_stateless_fdset(Ref) ->
    {ok, Socket} = gen_tcp:listen(0, [binary, {active,false}]),
    {ok, Port} = inet:port(Socket),
    {ok, FD} = inet:getfd(Socket),

    ok = inert:fdset(Ref, FD, [{mode, read_write}]),
    ok = inert:fdset(Ref, FD, [{mode, write}]),

    {ok, Conn} = gen_tcp:connect("localhost", Port, [binary]),
    ok = gen_tcp:close(Conn),

    ok = receive
        {inert_read, _, FD} = Fail ->
            Fail
    after
        0 ->
            ok
    end,

    ok = inert:fdset(Ref, FD, [{mode, read}]),

    Result = receive
        {inert_read, _, FD} = N ->
            N
    end,
    ?_assertMatch({inert_read, _, FD}, Result).

% Pass port ownership through a ring of processes
inert_controlling_process(Ref) ->
    {ok, Socket} = gen_udp:open(0, [binary, {active,false}]),
    {ok, FD} = inet:getfd(Socket),
    Pid = self(),
    inert_controlling_process(Ref, Pid, FD, 0),
    Result = receive
        inert_controlling_process ->
            N = inert:poll(Ref, FD, [{mode, write}]),
            gen_udp:close(Socket),
            N
    end,
    ?_assertEqual(ok, Result).

inert_controlling_process(Ref, Parent, _FD, 3) ->
    ok = inert:controlling_process(Ref, Parent),
    Parent ! inert_controlling_process;
inert_controlling_process(Ref, Parent, FD, N) ->
    ok = inert:poll(Ref, FD, [{mode, write}]),
    Pid1 = spawn(fun() -> inert_controlling_process(Ref, Parent, FD, N+1) end),
    ok = inert:controlling_process(Ref, Pid1).

% Catch the badarg if the port has been closed
inert_error_closed() ->
    {ok, Port} = inert:start(),
    ok = inert:stop(Port),
    ?_assertEqual({error, closed}, inert:poll(Port, 1)).

getenv(Var, Default) when is_list(Var), is_integer(Default) ->
    case os:getenv(Var) of
        false -> Default;
        N -> list_to_integer(N)
    end.
