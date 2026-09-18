-- An oper for the harness: "Admin", password "testpass", from any host.
-- The password is an 8 character salt and the md5 of salt + password, as
-- mod.ccontrol's LOGIN checks it.
INSERT INTO opers (user_name, password, access, saccess, last_updated, iscoder)
VALUES ('Admin', 'harness12a7d37cde62f9d6898588b2e6e5fdf71', 2147483647, 2147483647,
        date_part('epoch', CURRENT_TIMESTAMP)::int, 't');
INSERT INTO hosts (user_id, host)
SELECT user_id, '*!*@*' FROM opers WHERE user_name = 'Admin';
