//Put all assembly-level attributes in this file
//Note : some of the standard assembly info is generated by the build

using System.Security.Permissions;

//VSWhidbey 128447 - FxCop requires assembly-level permission requests
#pragma warning disable 618
[assembly:SecurityPermissionAttribute(SecurityAction.RequestMinimum, UnmanagedCode=true)]
#pragma warning restore 618

