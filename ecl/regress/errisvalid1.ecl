/*##############################################################################

    Copyright (C) 2011 HPCC Systems.

    All rights reserved. This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
############################################################################## */

import DMS_Test;
//loadxml('<section><ditem><val>dms_per_id</val></ditem></section>');
loadxml('<section><ditem><val>calcAge</val></ditem></section>');

#DECLARE (flag)

#SET(flag, #ISVALID('person.dms_per_id'))
#APPEND(flag, ' ')
#APPEND(flag, #ISVALID('DMS_Test.calcAge(0)'))

export out1 := %'flag'%;
out1;