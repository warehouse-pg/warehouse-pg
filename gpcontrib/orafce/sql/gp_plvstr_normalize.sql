-- GPDB (PTT-1923): plvstr.normalize() must drop a bare control byte in a
-- multibyte database encoding and keep normalizing the rest of the string,
-- not stop output at the first one.
SELECT plvstr.normalize('ab' || chr(1) || 'cd  ef') AS mid;
SELECT plvstr.normalize(chr(7) || '  x   y') AS leading;
SELECT plvstr.normalize('x' || chr(1) || chr(2) || chr(3) || ' y' || chr(4)) AS several;
SELECT plvstr.normalize('あ' || chr(7) || 'い  う') AS multibyte;
