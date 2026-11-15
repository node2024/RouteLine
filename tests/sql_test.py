"""Execute the production SR selection SQL on SQLite, including bans/styles/category filters."""
import sqlite3
import subprocess
import sys

db = sqlite3.connect(":memory:")
db.executescript("""
CREATE TABLE Maps(ID INTEGER, Name TEXT);
CREATE TABLE MapCourses(ID INTEGER, MapID INTEGER, Name TEXT);
CREATE TABLE Modes(ID INTEGER, ShortName TEXT);
CREATE TABLE Players(SteamID64 INTEGER, Alias TEXT);
CREATE TABLE Bans(ID INTEGER, SteamID64 INTEGER, ExpiresAt TEXT);
CREATE TABLE Times(ID TEXT, SteamID64 INTEGER, MapCourseID INTEGER, ModeID INTEGER,
                   StyleIDFlags INTEGER, RunTime REAL, Teleports INTEGER);
INSERT INTO Maps VALUES(1,'kz_test'),(2,'other_map');
INSERT INTO MapCourses VALUES(1,1,'Main Course'),(2,1,'bonus'),(3,2,'Main Course');
INSERT INTO Modes VALUES(1,'CKZ'),(2,'VNL');
INSERT INTO Players VALUES(1,'pro'),(2,'overall'),(3,'banned'),(4,'styled'),(5,'other mode'),(6,'other course'),(7,'other map'),(8,'third');
INSERT INTO Bans VALUES(1,3,NULL),(2,2,'2000-01-01 00:00:00');
INSERT INTO Times VALUES('pro',1,1,1,0,20.5,0),('overall',2,1,1,0,19.0,1),
 ('banned',3,1,1,0,1.0,0),('styled',4,1,1,1,2.0,0),('vnl',5,1,2,0,3.0,0),
 ('bonus',6,2,1,0,4.0,0),('other_map',7,3,1,0,5.0,0),
 ('pro-repeat',1,1,1,0,21.5,0),('pro-tie',1,1,1,0,20.5,0),
 ('overall-repeat',2,1,1,0,19.5,1),('overall-pro',2,1,1,0,25.5,0),('third',8,1,1,0,22.0,0);
""")
for category, expected in [("pro", "pro"), ("overall", "overall")]:
    sql = subprocess.check_output([sys.argv[1], "--sql", category], text=True)
    rows = db.execute(sql).fetchall()
    assert len(rows) == 1 and rows[0][0] == expected, rows
for category, rank, expected in [("pro", 2, "third"), ("overall", 2, "pro"), ("pro", 3, "overall-pro"), ("overall", 3, "third"), ("pro", 4, None)]:
    sql = subprocess.check_output([sys.argv[1], "--sql", category, str(rank)], text=True)
    row = db.execute(sql).fetchone()
    assert (row[0] if row else None) == expected, row
db.execute("INSERT INTO Bans VALUES(3,2,'2999-01-01 00:00:00')")
sql = subprocess.check_output([sys.argv[1], "--sql", "overall"], text=True)
assert db.execute(sql).fetchone()[0] == "pro"
print("SQLite SR selection: PRO/overall, active/expired bans, styles, map, course and mode: passed")
