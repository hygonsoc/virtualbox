<xsl:stylesheet version = '1.0'
     xmlns:xsl='http://www.w3.org/1999/XSL/Transform'
     xmlns:vbox="http://www.virtualbox.org/">

<!--

    constants-python.xsl:
        XSLT stylesheet that generates VirtualBox_constants.py from
        VirtualBox.xidl.

     Copyright (C) 2009 Sun Microsystems, Inc.

     This file is part of VirtualBox Open Source Edition (OSE), as
     available from http://www.virtualbox.org. This file is free software;
     you can redistribute it and/or modify it under the terms of the GNU
     General Public License (GPL) as published by the Free Software
     Foundation, in version 2 as it comes in the "COPYING" file of the
     VirtualBox OSE distribution. VirtualBox OSE is distributed in the
     hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.

     Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa
     Clara, CA 95054 USA or visit http://www.sun.com if you need
     additional information or have any questions.
-->

<xsl:output
  method="text"
  version="1.0"
  encoding="utf-8"
  indent="no"/>

<xsl:template match="/">
<xsl:text># Copyright (C) 2009 Sun Microsystems, Inc.
#
# This file is part of VirtualBox Open Source Edition (OSE), as
# available from http://www.virtualbox.org. This file is free software;
# you can redistribute it and/or modify it under the terms of the GNU
# General Public License (GPL) as published by the Free Software
# Foundation, in version 2 as it comes in the "COPYING" file of the
# VirtualBox OSE distribution. VirtualBox OSE is distributed in the
# hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
#
# Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa
# Clara, CA 95054 USA or visit http://www.sun.com if you need
# additional information or have any questions.
#
# This file is autogenerated from VirtualBox.xidl, DO NOT EDIT!
#
</xsl:text>
class VirtualBoxReflectionInfo:
   def __init__(self, isSym):
      self.isSym = isSym

   _Values = {<xsl:for-each select="//enum">
                '<xsl:value-of select="@name"/>':{
                  <xsl:for-each select="const">'<xsl:value-of select="@name"/>':<xsl:value-of select="@value"/><xsl:if test="not(position()=last())">,</xsl:if>
                  </xsl:for-each>}<xsl:if test="not(position()=last())">,</xsl:if>

              </xsl:for-each>}

   _ValuesFlat = {<xsl:for-each select="//enum">
                   <xsl:variable name="ename">
                    <xsl:value-of select="@name"/>
                   </xsl:variable>
                   <xsl:for-each select="const">
                        '<xsl:value-of select="$ename"/>_<xsl:value-of select="@name"/>':<xsl:value-of select="@value"/><xsl:if test="not(position()=last())">,</xsl:if>
                   </xsl:for-each>
                   <xsl:if test="not(position()=last())">,</xsl:if>
                  </xsl:for-each>}

   _ValuesFlatSym = {<xsl:for-each select="//enum">
                   <xsl:variable name="ename">
                    <xsl:value-of select="@name"/>
                   </xsl:variable>
                   <xsl:for-each select="const">
                     <xsl:variable name="eval">
                       <xsl:value-of select="concat($ename, '_', @name)"/>
                   </xsl:variable>
                        '<xsl:value-of select="$eval"/>': '<xsl:value-of select="@name"/>'<xsl:if test="not(position()=last())">,</xsl:if>
                   </xsl:for-each>
                   <xsl:if test="not(position()=last())">,</xsl:if>
                  </xsl:for-each>}

   def __getattr__(self,attr):
      if self.isSym:
        v = self._ValuesFlatSym.get(attr)
      else:
        v = self._ValuesFlat.get(attr)
      if v is not None:
         return v
      else:
         raise AttributeError

</xsl:template>
</xsl:stylesheet>
